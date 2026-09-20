#pragma once

#include <limits>
#include <string>
#include <vector>

#include <usdprep/Report.h>

namespace usdprep {

// "Use the stage's own frame" for the animation options below.
constexpr double kStageFrame = std::numeric_limits<double>::quiet_NaN();

struct ExtractOptions {
    std::vector<std::string> primPaths;  // subtree roots to keep
    std::string outputPath;              // .usda / .usdc / .usdz
    bool deinstance = true;              // drop instanceable flags in output
    bool setDefaultPrim = true;          // author defaultPrim on the output
    bool relinkTextures = true;          // .usdc/.usda: copy textures next to
                                         // the output and repoint the paths
    // Categories removed from the extracted result, e.g. the preview cards
    // and proxy geometry a production asset carries (Select's vocabulary).
    std::vector<std::string> dropTypes;
    std::vector<std::string> dropPurposes;
    // The material diet. materialPurpose: "preview" keeps the light
    // material where a prim has one for preview and one for full renders,
    // "full" the heavy one, "all" both. Render-context outputs
    // (outputs:arnold:*) and the shaders only they reach, and materials
    // nothing binds, go when the two flags are set.
    std::string materialPurpose = "all";
    bool stripRenderContexts = true;
    bool stripUnusedMaterials = true;
    // Animation: "all" leaves it, "range" drops time samples outside
    // [frameStart, frameEnd] (the stage's own range when kStageFrame),
    // "static" bakes staticFrame (the range start when kStageFrame) as
    // the only value.
    std::string animation = "all";
    double frameStart = kStageFrame;
    double frameEnd = kStageFrame;
    double staticFrame = kStageFrame;
};

// Copy the given subtrees (with their ancestors and carried dependencies)
// into a new standalone, flattened file. The input file is never modified.
//
// v0 behavior notes (validated in M0):
// - Materials living inside the extracted subtrees survive; materials bound
//   from outside the mask are dropped together with their bindings' targets
//   (dependency curation is the next milestone).
// - .usdz output localizes referenced textures (incl. UDIM tiles) into the
//   package; .usdc/.usda output copies them into a "<name>_textures" folder
//   next to the file and rewrites the paths (relinkTextures).
Report ExtractPrims(const std::string& inputPath, const ExtractOptions& options);

}  // namespace usdprep
