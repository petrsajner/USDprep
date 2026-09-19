#include <usdprep/Extract.h>

#include <string>
#include <vector>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stagePopulationMask.h>

#include "Shared.h"

namespace usdprep {

Report ExtractPrims(const std::string& inputPath, const ExtractOptions& options) {
    using namespace detail;

    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = options.outputPath;

    if (options.primPaths.empty()) {
        rep.Fail("no prim paths given");
        return rep;
    }
    if (options.outputPath.empty()) {
        rep.Fail("no output path given");
        return rep;
    }

    // Full open for the before-numbers and path validation.
    UsdStageRefPtr full = UsdStage::Open(inputPath, UsdStage::LoadAll);
    if (!full) {
        rep.Fail("cannot open stage: " + inputPath);
        return rep;
    }
    rep.before = InspectStage(inputPath).counts;
    const std::vector<SdfPath> roots = ValidatePrimPaths(full, options.primPaths, rep);
    if (!rep.error.empty()) return rep;

    // Masked open: population limited to the requested subtrees.
    UsdStageRefPtr stage = UsdStage::OpenMasked(
        inputPath, UsdStagePopulationMask(roots), UsdStage::LoadAll);
    if (!stage) {
        rep.Fail("cannot open masked stage: " + inputPath);
        return rep;
    }
    rep.Info("extract",
             std::to_string(roots.size()) + " subtree(s) selected; composition flattened");

    if (options.deinstance) {
        const size_t n = DeinstanceStage(stage);
        if (n > 0) {
            rep.Info("de-instance",
                     std::to_string(n) + " instanceable prim(s) converted to plain prims");
        }
    }

    if (const size_t anchored = AnchorUnresolvedAssetPaths(stage)) {
        rep.Info("textures", std::to_string(anchored) +
                                 " texture path(s) that no resolver can expand (UDIM tile "
                                 "sets) anchored to their source folder");
    }

    const std::string tmpPath = TempPathFor(options.outputPath);
    if (!stage->Export(tmpPath, /*addSourceFileComment=*/false)) {
        rep.Fail("failed to export flattened layer to " + tmpPath);
        return rep;
    }

    // Post-pass on the flattened output: drop the categories the recipe
    // asks for, then author defaultPrim if missing.
    const bool hasFilters = !options.dropTypes.empty() || !options.dropPurposes.empty();
    if (options.setDefaultPrim || hasFilters) {
        const UsdStageRefPtr flat = UsdStage::Open(tmpPath);
        if (!flat) {
            rep.Fail("cannot reopen the flattened layer: " + tmpPath);
            return rep;
        }
        DropCategoriesFromStage(rep, flat, options.dropTypes, options.dropPurposes);
        if (options.setDefaultPrim && AuthorDefaultPrim(flat, roots.front())) {
            rep.Info("defaultPrim",
                     "set to top-level ancestor of " + roots.front().GetAsString());
        }
        flat->Save();
    }

    FinalizeOutput(rep, options.outputPath, tmpPath, options.relinkTextures);
    return rep;
}

}  // namespace usdprep
