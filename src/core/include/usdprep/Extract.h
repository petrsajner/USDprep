#pragma once

#include <limits>
#include <string>
#include <vector>

#include <usdprep/Progress.h>
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
    // Draw-mode cards (a viewer's stand-in box with six textures) go too.
    bool stripDrawModeCards = true;
    // Nuke does not read UDIM tile sets: stitch each multi-tile set into one
    // texture and squeeze the UVs into it inside the material.
    bool udimAtlas = true;
    // Whatever is left that Nuke cannot read is converted or replaced and
    // reported (NUKE_COMPAT.md): kept guide/proxy geometry is hidden,
    // MaterialX outputs next to a standard surface go, meshes are unbound
    // from materials Nuke cannot render, unsupported lights become axes.
    bool nukeCompat = true;
    // Textures larger than this on their longer side are scaled down in
    // the output (0 = leave them alone). The originals are never touched.
    int maxTextureSize = 0;
    // Decimate meshes to this share of their triangles (0 < ratio < 1);
    // 0 leaves the geometry alone. Opt-in, in every preset.
    double simplifyRatio = 0.0;
    // Animation: "all" leaves it, "range" drops time samples outside
    // [frameStart, frameEnd] (the stage's own range when kStageFrame),
    // "static" bakes staticFrame (the range start when kStageFrame) as
    // the only value.
    std::string animation = "all";
    double frameStart = kStageFrame;
    double frameEnd = kStageFrame;
    double staticFrame = kStageFrame;
    // Optional: where the run reports how far it is, and where it is told
    // to stop (it then fails with the error "cancelled" and leaves nothing
    // behind). Owned by the caller, alive for the whole run.
    Progress* progress = nullptr;
};

// Copy the given subtrees (with their ancestors and carried dependencies)
// into a new standalone, flattened file. The input file is never modified.
//
// Behaviour notes:
// - The materials the subtrees are bound to come along, also when they
//   live elsewhere in the scene, with the shader nodes their networks reach.
// - .usdz output localizes referenced textures (incl. UDIM tiles) into the
//   package; .usdc/.usda output copies them into a "<name>_textures" folder
//   next to the file and rewrites the paths (relinkTextures).
Report ExtractPrims(const std::string& inputPath, const ExtractOptions& options);

}  // namespace usdprep
