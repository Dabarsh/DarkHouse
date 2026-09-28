// DarkHouse — per-user application directories.
#pragma once

#include <filesystem>

namespace darkhouse {

// Where DarkHouse keeps per-user settings such as the UI layout:
//   Linux/BSD: $XDG_CONFIG_HOME/DarkHouse, else ~/.config/DarkHouse
//   macOS:     ~/Library/Application Support/DarkHouse
//   Windows:   %APPDATA%\DarkHouse
// Falls back to the current directory when none of those can be determined.
// The directory is not created here.
[[nodiscard]] std::filesystem::path userConfigDirectory();

}  // namespace darkhouse
