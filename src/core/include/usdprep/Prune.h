#pragma once

#include <string>
#include <vector>

#include <usdprep/Extract.h>

namespace usdprep {

struct PruneOptions {
    // Exactly one mode is requested: keep-only (keepPaths) or drop
    // (dropPaths and/or the drop filters below).
    std::vector<std::string> keepPaths;  // keep these subtrees, drop the rest
    std::vector<std::string> dropPaths;  // drop these subtrees, keep the rest
    // Drop by category instead of by path: prim types (e.g. "light",
    // "Camera") and resolved purposes (e.g. "guide", "proxy"). Resolved
    // against the input stage through Select and added to dropPaths.
    std::vector<std::string> dropTypes;
    std::vector<std::string> dropPurposes;
    std::string outputPath;
    bool deinstance = true;
    bool setDefaultPrim = true;
};

// Shrink a scene: either keep-only (implemented as a masked extraction) or
// drop-selection (flatten, then delete the subtrees from the flattened
// layer). The input file is never modified.
Report PruneStage(const std::string& inputPath, const PruneOptions& options);

}  // namespace usdprep
