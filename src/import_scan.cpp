#include "import_scan.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <system_error>

namespace darkhouse {

namespace fs = std::filesystem;

bool isSupportedImage(const fs::path& path) {
    static constexpr std::string_view kExtensions[] = {
        ".jpg", ".jpeg", ".png", ".tif", ".tiff", ".dng", ".cr2", ".cr3", ".nef", ".nrw", ".arw",
        ".orf", ".rw2", ".raf", ".pef", ".srw", ".heic", ".heif", ".avif", ".webp", ".exr"};
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

ImportScan scanImportPaths(const std::vector<std::string>& arguments) {
    ImportScan scan;
    for (const std::string& argument : arguments) {
        std::error_code ec;
        const fs::path path = fs::absolute(argument, ec);
        if (ec) {
            scan.warnings.push_back("cannot resolve '" + argument + "': " + ec.message());
            continue;
        }
        if (fs::is_directory(path, ec)) {
            const auto options = fs::directory_options::skip_permission_denied;
            for (auto it = fs::recursive_directory_iterator(path, options, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec) && isSupportedImage(it->path())) scan.files.push_back(it->path().string());
            }
            if (ec) scan.warnings.push_back("stopped scanning '" + path.string() + "': " + ec.message());
        } else {
            scan.files.push_back(path.string());
        }
    }
    return scan;
}

}  // namespace darkhouse
