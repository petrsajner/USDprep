// Where the exported file goes, and whether that answer makes sense.
//
// Pure string/filesystem work, deliberately free of ImGui and usdtweak so
// tests/test_outputpath.cpp can hold it to its word: this is the code that
// once turned an empty name into a file called ".usdz.usdz".
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace usdprep_addon {

inline std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

inline std::string BasenameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ".usdz" on its own is an extension, not a file called ".usdz" — hence
// >= and not >. Get this wrong and WithExtension happily produces
// ".usdz.usdz".
inline bool HasUsdExtension(const std::string& path) {
    return path.size() >= 5 &&
           (path.compare(path.size() - 5, 5, ".usdz") == 0 ||
            path.compare(path.size() - 5, 5, ".usdc") == 0 ||
            path.compare(path.size() - 5, 5, ".usda") == 0);
}

// Length of the extension when it is one of ours (.usdc/.usda/.usdz, .obj), else 0.
inline size_t KnownExtensionLength(const std::string& path) {
    if (HasUsdExtension(path)) return 5;
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".obj") == 0) return 4;
    return 0;
}

inline std::string WithExtension(const std::string& path, const char* ext) {
    std::string p = path;
    p.resize(p.size() - KnownExtensionLength(p));
    return p + ext;
}

// What the exported file is actually called: no directory, no extension.
// Empty means the artist has not named it yet.
inline std::string OutputNameOf(const std::string& path) {
    std::string name = BasenameOf(path);
    name.resize(name.size() - KnownExtensionLength(name));
    return name;
}

// Why the export cannot run yet, in the artist's words. Empty = it can.
inline std::string ExportBlocker(bool hasSelection, const std::string& outputPath) {
    if (!hasSelection) {
        return "Check at least one object above, or click one in the 3D view.";
    }
    if (OutputNameOf(outputPath).empty()) {
        return "Give the exported file a name.";
    }
    // An empty directory part means the path is bare ("car.usdz") or
    // rooted ("/car.usdz") — nothing to check in either case.
    const std::string dir = DirectoryOf(outputPath);
    std::error_code ec;
    if (!dir.empty() && dir != "." && !std::filesystem::is_directory(dir, ec)) {
        return "There is no folder '" + dir + "'.";
    }
    return {};
}

}  // namespace usdprep_addon
