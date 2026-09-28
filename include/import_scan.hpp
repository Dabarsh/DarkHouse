// DarkHouse — expanding user-supplied import paths into files.
//
// Shared by the command line (--import) and the UI's Import dialog.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace darkhouse {

// True for extensions the importer understands (RAW, JPEG, TIFF, PNG, HEIF, ...).
[[nodiscard]] bool isSupportedImage(const std::filesystem::path& path);

struct ImportScan {
    std::vector<std::string> files;     // absolute paths, ready for ImportFilesEvent
    std::vector<std::string> warnings;  // unresolvable arguments, unreadable directories
};

// Turns files and directories into absolute file paths. Directories are walked
// recursively and only files with image extensions are kept. Explicit file
// arguments are passed through unchecked; the importer reports missing or
// unreadable files. Can take a while on large trees, so the UI runs it off the
// main thread.
[[nodiscard]] ImportScan scanImportPaths(const std::vector<std::string>& arguments);

}  // namespace darkhouse
