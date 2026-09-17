#pragma once

#include <string>
#include <vector>

#include <usdprep/Extract.h>

namespace usdprep {

struct PruneOptions {
    // Exactly one group is non-empty:
    std::vector<std::string> keepPaths;  // keep these subtrees, drop the rest
    std::vector<std::string> dropPaths;  // drop these subtrees, keep the rest
    std::string outputPath;
    bool deinstance = true;
    bool setDefaultPrim = true;
};

// Shrink a scene: either keep-only (implemented as a masked extraction) or
// drop-selection (flatten, then delete the subtrees from the flattened
// layer). The input file is never modified.
Report PruneStage(const std::string& inputPath, const PruneOptions& options);

}  // namespace usdprep
