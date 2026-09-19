#pragma once

#include <string>
#include <vector>

#include <pxr/usd/usd/common.h>

namespace usdprep {

// Filters that turn a stage into a list of prim paths. An empty filter
// matches everything; filters are ANDed, entries inside one filter are ORed.
struct SelectOptions {
    std::vector<std::string> roots;  // search only under these subtrees
    // Prim types, case-insensitive: concrete schema names ("Mesh", "Camera",
    // "SphereLight") plus the family name "light" (anything carrying
    // UsdLuxLightAPI, whatever the concrete light type is).
    std::vector<std::string> types;
    // Prim name: plain text means "contains" (what artists type); a pattern
    // with * or ? is matched as a wildcard over the whole name. Always
    // case-insensitive.
    std::string namePattern;
    // Resolved purpose: default | render | proxy | guide. Inherited from
    // ancestors the way USD computes it, so a mesh under a guide group is
    // guide. Non-imageable prims never match a purpose filter.
    std::vector<std::string> purposes;
    // Keep only matches that have no matching ancestor — the set you would
    // hand to Extract/Prune as subtree roots.
    bool topmostOnly = false;
};

struct SelectResult {
    std::vector<std::string> paths;  // matches, in stage order
    size_t visited = 0;              // prims examined
    std::string error;               // non-empty: nothing was searched
};

// Find the prims matching `options`. The stage is only read.
SelectResult SelectPrims(const std::string& inputPath, const SelectOptions& options);

// Same, on a stage the caller already has open (the GUI path — a heavy
// stage is opened once and filtered many times).
SelectResult SelectPrims(const pxr::UsdStageRefPtr& stage, const SelectOptions& options);

}  // namespace usdprep
