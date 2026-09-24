// Files USDprep opens by turning them into USD first.
//
// The conversion writes a USD file - in the program, a temporary one -
// and everything after that is the same as for any USD scene: pick,
// reduce, export. The source file is only read.
#pragma once

#include <atomic>
#include <string>

#include <usdprep/Report.h>

namespace usdprep {

// True when `path` names a file USDprep imports (by its extension): .obj.
bool IsImportable(const std::string& path);

// A running import, shared with whoever shows it.
struct ImportProgress {
    std::atomic<float> fraction{0.0f};  // 0..1
    std::atomic<bool> cancel{false};    // set by the caller: stop as soon as possible
};

// Converts `inputPath` (see IsImportable) into the USD file `usdPath`
// (.usdc or .usda). The report says what was read, what changed on the
// way and what could not come along. Paths are UTF-8.
Report ImportToUsd(const std::string& inputPath, const std::string& usdPath, ImportProgress* progress = nullptr);

// customLayerData key of an imported layer: the file it was made from.
constexpr const char* kImportedFromKey = "usdprep:importedFrom";

}  // namespace usdprep
