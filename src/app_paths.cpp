#include "app_paths.hpp"

#include <cstdlib>

namespace darkhouse {
namespace {

std::filesystem::path fromEnv(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}

}  // namespace

std::filesystem::path userConfigDirectory() {
#if defined(_WIN32)
    const std::filesystem::path base = fromEnv("APPDATA");
#elif defined(__APPLE__)
    const std::filesystem::path home = fromEnv("HOME");
    const std::filesystem::path base = home.empty() ? home : home / "Library" / "Application Support";
#else
    std::filesystem::path base = fromEnv("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::filesystem::path home = fromEnv("HOME");
        if (!home.empty()) base = home / ".config";
    }
#endif
    return base.empty() ? std::filesystem::path("DarkHouse") : base / "DarkHouse";
}

}  // namespace darkhouse
