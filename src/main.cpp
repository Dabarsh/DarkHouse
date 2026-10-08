// DarkHouse entry point: parses the command line, wires signals and runs the
// application controller.

#include "app_controller.hpp"
#include "app_paths.hpp"
#include "import_scan.hpp"

#ifdef DARKHOUSE_WITH_GUI
#include "gui_engine.hpp"
#include "ui/shell.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef DARKHOUSE_VERSION
#define DARKHOUSE_VERSION "dev"
#endif
#ifndef DARKHOUSE_VALIDATION_DEFAULT
#define DARKHOUSE_VALIDATION_DEFAULT 0
#endif

namespace fs = std::filesystem;
using namespace darkhouse;

namespace {

std::atomic<DarkHouseApp*> g_app{nullptr};

void onInterrupt(int) {
    if (DarkHouseApp* app = g_app.load()) app->requestQuit();
}

void printUsage(std::ostream& out) {
    out << "DarkHouse " DARKHOUSE_VERSION " - RAW development, asset management, compositing and vector design\n"
           "\n"
           "Usage: DarkHouse [options]\n"
           "\n"
           "Opens the desktop UI unless --headless is given (or no display is available).\n"
           "\n"
           "  --catalog <file>        catalog database (default: darkhouse_catalog.sqlite)\n"
           "  --import <path>...      import files; directories are scanned recursively\n"
           "  --query <filter>        after the run, list assets matching a SQL filter over aliases\n"
           "                          a (assets) and m (metadata), e.g. \"a.rating >= 3 AND m.iso <= 800\"\n"
           "                          (use \"\" to list everything)\n"
           "  --rate <asset-id> <0-5> set an asset's star rating\n"
           "  --open <asset-id>       open an asset on the canvas: its photo and develop stack\n"
           "  --mode <catalog|develop|canvas|split>\n"
           "  --canvas <W>x<H>        empty document size before a photo is opened (default 2048x2048)\n"
           "  --preview-size <px>     longest edge of an opened photo on the canvas; larger files are\n"
           "                          downscaled for interactive editing (default 3072, 0 = full size)\n"
           "  --shaders <dir>         directory containing compiled *.spv shaders\n"
           "  --models <dir>          directory containing *_segmentation.onnx models\n"
           "  --lens-db <dir>         lensfun lens database (*.xml) for lens profile corrections\n"
           "                          (default: $DARKHOUSE_LENSFUN_DIR, then /usr/share/lensfun/version_1)\n"
           "  --frames <n>            run exactly n frames, then exit (headless: instead of exiting when idle)\n"
           "  --headless              no window: run the queued work and exit once it drains\n"
           "  --window <W>x<H>        initial window size (default: 85% of the screen)\n"
           "  --maximized             open the window maximized\n"
           "  --no-vsync              present without waiting for the display refresh\n"
           "  --continuous            redraw every frame even when idle (benchmarks, capture)\n"
           "  --viewports             allow dragging panels out into their own OS windows\n"
           "  --layout <file>         UI layout file (default: <user config dir>/DarkHouse/layout.ini)\n"
           "  --no-gpu                skip Vulkan initialization (implies --headless)\n"
           "  --validation            enable Vulkan validation layers (default in Debug builds;\n"
           "                          env DARKHOUSE_VULKAN_VALIDATION=0/1 overrides the build default)\n"
           "  --no-validation         disable Vulkan validation layers\n"
           "  --version, --help\n";
}

// Unix seconds -> "YYYY-MM-DD HH:MM" (UTC), using Hinnant's civil_from_days.
std::string formatTimestamp(std::int64_t seconds) {
    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2);

    char buffer[96];  // sized for the full int64 range, which keeps -Wformat-truncation quiet
    std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02u %02lld:%02lld", static_cast<long long>(year), month, day,
                  static_cast<long long>(rem / 3600), static_cast<long long>(rem % 3600 / 60));
    return buffer;
}

void printAssets(const std::vector<AssetRecord>& assets) {
    std::cout << assets.size() << " asset(s)\n";
    for (const AssetRecord& a : assets) {
        std::cout << a.id << "  " << std::string(static_cast<std::size_t>(a.rating), '*')
                  << std::string(static_cast<std::size_t>(5 - a.rating), '.') << "  "
                  << (a.dateCaptured ? formatTimestamp(*a.dateCaptured) : std::string("----------------")) << "  "
                  << std::setw(11) << (std::to_string(a.width) + "x" + std::to_string(a.height)) << "  "
                  << std::left << std::setw(22)
                  << a.metadata.cameraModel.value_or("-").substr(0, 22) << std::right << "  "
                  << (a.metadata.iso ? "ISO " + std::to_string(*a.metadata.iso) : std::string("-")) << "  "
                  << a.filePath << '\n';
    }
}

struct CommandLine {
    AppConfig config;
    std::vector<std::string> importArguments;
    std::optional<std::string> query;
    std::vector<std::pair<std::string, int>> ratings;
    std::optional<std::string> openAssetId;
    bool headless = false;
    int windowWidth = 0;   // 0 = size from the monitor's work area
    int windowHeight = 0;
    bool maximized = false;
    bool vsync = true;
    bool continuous = false;
    bool multiViewport = false;
    std::optional<std::filesystem::path> layoutFile;
    bool help = false;
    bool version = false;
};

std::optional<CommandLine> parseCommandLine(int argc, char** argv) {
    CommandLine cli;
    cli.config.enableValidationLayers = DARKHOUSE_VALIDATION_DEFAULT != 0;
    if (const char* validation = std::getenv("DARKHOUSE_VULKAN_VALIDATION"); validation && *validation) {
        cli.config.enableValidationLayers = std::string_view(validation) != "0";
    }
    if (const char* shaders = std::getenv("DARKHOUSE_SHADER_DIR")) {
        cli.config.shaderDirectory = shaders;
    }
#ifdef DARKHOUSE_SHADER_DIR
    else {
        cli.config.shaderDirectory = DARKHOUSE_SHADER_DIR;
    }
#endif

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    auto fail = [](const std::string& message) -> std::optional<CommandLine> {
        std::cerr << "error: " << message << "\n\n";
        printUsage(std::cerr);
        return std::nullopt;
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) return std::nullopt;
            return std::string(args[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            cli.help = true;
        } else if (arg == "--version") {
            cli.version = true;
        } else if (arg == "--catalog") {
            const auto v = value();
            if (!v) return fail("--catalog needs a file");
            cli.config.catalogPath = *v;
        } else if (arg == "--import") {
            while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) cli.importArguments.emplace_back(args[++i]);
            if (cli.importArguments.empty()) return fail("--import needs at least one path");
        } else if (arg == "--query") {
            const auto v = value();
            if (!v) return fail("--query needs a filter (\"\" for all assets)");
            cli.query = *v;
        } else if (arg == "--rate") {
            const auto id = value();
            const auto stars = value();
            if (!id || !stars || stars->size() != 1 || (*stars)[0] < '0' || (*stars)[0] > '5') {
                return fail("--rate needs <asset-id> <0-5>");
            }
            cli.ratings.emplace_back(*id, (*stars)[0] - '0');
        } else if (arg == "--open") {
            const auto v = value();
            if (!v) return fail("--open needs an asset id");
            cli.openAssetId = *v;
        } else if (arg == "--mode") {
            const auto v = value();
            const auto mode = v ? parseAppMode(*v) : std::nullopt;
            if (!mode) return fail("--mode must be catalog, develop, canvas or split");
            cli.config.initialMode = *mode;
        } else if (arg == "--canvas") {
            const auto v = value();
            unsigned w = 0, h = 0;
            if (!v || std::sscanf(v->c_str(), "%ux%u", &w, &h) != 2 || w == 0 || h == 0 || w > 65536 || h > 65536) {
                return fail("--canvas needs <W>x<H>, each 1..65536");
            }
            cli.config.canvasWidth = w;
            cli.config.canvasHeight = h;
        } else if (arg == "--preview-size") {
            const auto v = value();
            char* end = nullptr;
            const unsigned long size = v ? std::strtoul(v->c_str(), &end, 10) : 0;
            if (!v || v->empty() || *end != '\0' || size > 65536) return fail("--preview-size needs 0..65536 pixels");
            cli.config.previewMaxDimension = static_cast<std::uint32_t>(size);
        } else if (arg == "--shaders") {
            const auto v = value();
            if (!v) return fail("--shaders needs a directory");
            cli.config.shaderDirectory = *v;
        } else if (arg == "--models") {
            const auto v = value();
            if (!v) return fail("--models needs a directory");
            cli.config.modelDirectory = *v;
        } else if (arg == "--lens-db") {
            const auto v = value();
            if (!v) return fail("--lens-db needs a directory");
            cli.config.lensDatabaseDirectory = *v;
        } else if (arg == "--frames") {
            const auto v = value();
            char* end = nullptr;
            const unsigned long long frames = v ? std::strtoull(v->c_str(), &end, 10) : 0;
            if (!v || frames == 0 || *end != '\0') return fail("--frames needs a positive integer");
            cli.config.maxFrames = frames;
        } else if (arg == "--headless") {
            cli.headless = true;
        } else if (arg == "--window") {
            const auto v = value();
            unsigned w = 0, h = 0;
            if (!v || std::sscanf(v->c_str(), "%ux%u", &w, &h) != 2 || w < 640 || h < 400 || w > 16384 || h > 16384) {
                return fail("--window needs <W>x<H>, at least 640x400");
            }
            cli.windowWidth = static_cast<int>(w);
            cli.windowHeight = static_cast<int>(h);
        } else if (arg == "--maximized") {
            cli.maximized = true;
        } else if (arg == "--no-vsync") {
            cli.vsync = false;
        } else if (arg == "--continuous") {
            cli.continuous = true;
        } else if (arg == "--viewports") {
            cli.multiViewport = true;
        } else if (arg == "--layout") {
            const auto v = value();
            if (!v) return fail("--layout needs a file");
            cli.layoutFile = fs::path(*v);
        } else if (arg == "--no-gpu") {
            cli.config.enableGpu = false;
            cli.headless = true;  // the UI renders with Vulkan
        } else if (arg == "--validation") {
            cli.config.enableValidationLayers = true;
        } else if (arg == "--no-validation") {
            cli.config.enableValidationLayers = false;
        } else {
            return fail("unknown option '" + std::string(arg) + "'");
        }
    }

#ifndef DARKHOUSE_WITH_GUI
    cli.headless = true;  // built with -DDARKHOUSE_GUI=OFF
#endif
    // Headless runs do the requested work and exit once it drains, unless a
    // frame count is given. (The UI is interactive and never exits when idle.)
    cli.config.exitWhenIdle = !cli.config.maxFrames.has_value();
    return cli;
}

#ifdef DARKHOUSE_WITH_GUI
// Opens the DarkHouse window. Returns nullptr (and logs why) when no window
// can be created, e.g. over SSH without a display; the caller then runs headless.
std::unique_ptr<FrontEnd> createDesktopFrontEnd(const CommandLine& cli) {
    try {
        GuiOptions options;
        options.window.title = "DarkHouse";
        options.window.width = cli.windowWidth;
        options.window.height = cli.windowHeight;
        options.window.maximized = cli.maximized;
        options.vsync = cli.vsync;
        options.lowPowerIdle = !cli.continuous;
        options.multiViewport = cli.multiViewport;
        options.iniPath = cli.layoutFile.value_or(userConfigDirectory() / "layout.ini");
        return std::make_unique<GuiEngine>(std::move(options), std::make_unique<ui::DarkHouseShell>());
    } catch (const std::exception& e) {
        std::clog << "[DarkHouse] warn: cannot open the DarkHouse window (" << e.what() << "); running headless\n";
        return nullptr;
    }
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::optional<CommandLine> cli = parseCommandLine(argc, argv);
    if (!cli) return 64;  // EX_USAGE
    if (cli->help) {
        printUsage(std::cout);
        return 0;
    }
    if (cli->version) {
        std::cout << "DarkHouse " DARKHOUSE_VERSION "\n";
        return 0;
    }

    int exitCode = 0;
    try {
        DarkHouseApp app(cli->config);
#ifdef DARKHOUSE_WITH_GUI
        if (!cli->headless) {
            if (std::unique_ptr<FrontEnd> gui = createDesktopFrontEnd(*cli)) app.setFrontEnd(std::move(gui));
        }
#endif
        app.initialize();

        g_app.store(&app);
        std::signal(SIGINT, onInterrupt);
        std::signal(SIGTERM, onInterrupt);

        if (!cli->importArguments.empty()) {
            const ImportScan scan = scanImportPaths(cli->importArguments);
            for (const std::string& warning : scan.warnings) std::cerr << "warning: " << warning << '\n';
            const std::vector<std::string>& files = scan.files;
            std::clog << "[DarkHouse] info: queued " << files.size() << " file(s) for import\n";
            app.postEvent(ImportFilesEvent{files});
        }
        for (const auto& [assetId, rating] : cli->ratings) app.postEvent(SetRatingEvent{assetId, rating});
        if (cli->openAssetId) app.postEvent(OpenAssetEvent{*cli->openAssetId});

        exitCode = app.run();
        g_app.store(nullptr);
    } catch (const std::exception& e) {
        g_app.store(nullptr);
        std::cerr << "[DarkHouse] fatal: " << e.what() << '\n';
        return 1;
    }

    if (cli->query) {
        try {
            AssetManager catalog;
            catalog.initializeCatalog(cli->config.catalogPath.string());
            printAssets(catalog.queryAssets(*cli->query));
        } catch (const std::exception& e) {
            std::cerr << "[DarkHouse] query failed: " << e.what() << '\n';
            return 1;
        }
    }
    return exitCode;
}
