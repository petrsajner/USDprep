// Where an FBX's first animation starts (FbxScan.cpp): assimp hands the
// keys over moved to 0, the file's frame numbers are read here.
#pragma once

#include <filesystem>

namespace usdprep {
namespace importer {

// The start of the first animation stack in seconds - its LocalStart, else
// the earliest key of any curve. False when the file has no animation.
bool FbxAnimationStart(const std::filesystem::path& path, double* seconds);

}  // namespace importer
}  // namespace usdprep
