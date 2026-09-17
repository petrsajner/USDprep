#pragma once

#include <set>
#include <string>

#include <usdprep/Report.h>

namespace usdprep {

// Stage-level metadata gathered by `usdcut inspect`.
struct StageMetadata {
    std::string upAxis;
    double metersPerUnit = 0.0;
    std::string defaultPrim;
    double startTimeCode = 0.0;
    double endTimeCode = 0.0;
    double framesPerSecond = 0.0;
    std::set<std::string> textureExtensions;
};

struct StageInfo {
    Report::Counts counts;
    StageMetadata meta;
    uint64_t fileSizeBytes = 0;
};

// Open a stage (all payloads) and gather statistics. On failure returns
// counts of zero and, when `error` is given, writes a reason into it.
StageInfo InspectStage(const std::string& path, std::string* error = nullptr);

}  // namespace usdprep
