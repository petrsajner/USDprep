#include <usdprep/Prune.h>

#include <string>
#include <vector>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Select.h>

#include "Shared.h"

namespace usdprep {

namespace {

using namespace pxr;
using namespace detail;

// Drop subtrees that are already covered by an ancestor in the same set
// (deleting both /A and /A/B in one batch edit would be redundant).
std::vector<SdfPath> RemoveNestedPaths(std::vector<SdfPath> paths) {
    std::vector<SdfPath> result;
    for (const SdfPath& p : paths) {
        bool covered = false;
        for (const SdfPath& q : paths) {
            if (q != p && p.HasPrefix(q)) {
                covered = true;
                break;
            }
        }
        if (!covered) result.push_back(p);
    }
    return result;
}

void DoPrune(Report& rep, const std::string& inputPath, const PruneOptions& options) {
    rep.inputPath = inputPath;
    rep.outputPath = options.outputPath;

    const bool keepMode = !options.keepPaths.empty();
    const bool dropMode = !options.dropPaths.empty() || !options.dropTypes.empty() ||
                          !options.dropPurposes.empty();
    if (keepMode == dropMode) {  // both or neither
        rep.Fail("specify exactly one of --except (keep) or --drop");
        return;
    }
    if (options.outputPath.empty()) {
        rep.Fail("no output path given");
        return;
    }

    if (keepMode) {
        // Keep-only pruning is a masked extraction with several roots.
        ExtractOptions extractOptions;
        extractOptions.primPaths = options.keepPaths;
        extractOptions.outputPath = options.outputPath;
        extractOptions.deinstance = options.deinstance;
        extractOptions.setDefaultPrim = options.setDefaultPrim;
        extractOptions.relinkTextures = options.relinkTextures;
        extractOptions.materialPurpose = options.materialPurpose;
        extractOptions.stripRenderContexts = options.stripRenderContexts;
        extractOptions.stripUnusedMaterials = options.stripUnusedMaterials;
        extractOptions.stripDrawModeCards = options.stripDrawModeCards;
        extractOptions.udimAtlas = options.udimAtlas;
        extractOptions.nukeCompat = options.nukeCompat;
        extractOptions.maxTextureSize = options.maxTextureSize;
        extractOptions.simplifyRatio = options.simplifyRatio;
        extractOptions.animation = options.animation;
        extractOptions.frameStart = options.frameStart;
        extractOptions.frameEnd = options.frameEnd;
        extractOptions.staticFrame = options.staticFrame;
        Report r = ExtractPrims(inputPath, extractOptions);
        for (const auto& e : r.entries) {
            if (e.action == "extract") {
                r.entries.clear();
                r.Info("prune-keep",
                       std::to_string(options.keepPaths.size()) +
                           " subtree(s) kept, everything else dropped; "
                           "composition flattened");
                break;
            }
        }
        rep = r;
        return;
    }

    // Drop mode: flatten the whole stage first, then delete the selected
    // subtrees from the flattened single layer.
    UsdStageRefPtr stage = UsdStage::Open(inputPath, UsdStage::LoadAll);
    if (!stage) {
        rep.Fail("cannot open stage: " + inputPath);
        return;
    }
    rep.before = InspectStage(inputPath).counts;
    const std::string inputDefaultPrim =
        stage->GetDefaultPrim() ? stage->GetDefaultPrim().GetName().GetString() : "";

    const std::vector<SdfPath> roots =
        RemoveNestedPaths(ValidatePrimPaths(stage, options.dropPaths, rep));
    if (!rep.error.empty()) return;

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
        return;
    }
    rep.Info("flatten", "composition flattened to a single layer");

    // Reopen the flattened file and delete the selected subtrees. After a
    // flatten everything lives in the root layer, so RemovePrim edits the
    // output file only — never the user's input.
    UsdStageRefPtr flat = UsdStage::Open(tmpPath);
    if (!flat) {
        rep.Fail("cannot reopen flattened layer: " + tmpPath);
        return;
    }
    for (const SdfPath& p : roots) {
        const UsdPrim prim = flat->GetPrimAtPath(p);
        if (prim && prim.IsInstanceProxy()) {
            rep.Fail("cannot delete " + p.GetAsString() +
                     ": it lives inside instanced content shared by several "
                     "instances — re-run with de-instancing enabled");
            return;
        }
        if (!flat->RemovePrim(p)) {
            rep.Fail("failed to delete prim " + p.GetAsString());
            return;
        }
    }
    if (!roots.empty()) {
        rep.Info("prune-drop", std::to_string(roots.size()) +
                                   " subtree(s) deleted from the flattened layer");
    }
    DropCategoriesFromStage(rep, flat, options.dropTypes, options.dropPurposes);
    StripMaterials(rep, flat, options.materialPurpose, options.stripRenderContexts,
                   options.stripUnusedMaterials, options.udimAtlas);
    if (options.nukeCompat) MakeNukeReadable(rep, flat);
    if (options.udimAtlas) {
        AtlasUdimTextures(rep, flat, AtlasDirFor(options.outputPath, tmpPath, options.relinkTextures),
                          options.maxTextureSize);
    }
    if (options.stripDrawModeCards) StripDrawModeCards(rep, flat);
    TrimAnimation(rep, flat, options.animation, options.frameStart, options.frameEnd,
                  options.staticFrame);
    SimplifyMeshes(rep, flat, options.simplifyRatio);

    if (options.setDefaultPrim && !flat->GetDefaultPrim()) {
        // Prefer the input's own default prim when it survived the prune.
        UsdPrim chosen;
        if (!inputDefaultPrim.empty()) {
            chosen = flat->GetPrimAtPath(
                SdfPath::AbsoluteRootPath().AppendChild(TfToken(inputDefaultPrim)));
        }
        // Otherwise the first real top-level prim. Flattening a stage that
        // keeps its instancing emits "Flattened_Prototype_N" prims next to
        // the scene; pointing defaultPrim at one of those would make the
        // output open on a prototype instead of on the scene.
        if (!chosen) {
            for (const UsdPrim& prim : flat->GetPseudoRoot().GetAllChildren()) {
                if (prim.GetName().GetString().rfind("Flattened_Prototype", 0) == 0) continue;
                chosen = prim;
                break;
            }
        }
        if (chosen) {
            flat->SetDefaultPrim(chosen);
            rep.Info("defaultPrim", "set to " + chosen.GetPrimPath().GetAsString());
        } else {
            rep.Warn("defaultPrim", "output has no prim to point at — none authored");
        }
    }
    flat->Save();

    FinalizeOutput(rep, options.outputPath, tmpPath, options.relinkTextures,
                   options.maxTextureSize);
}

}  // namespace

Report PruneStage(const std::string& inputPath, const PruneOptions& options) {
    Report rep;
    if (!options.keepPaths.empty()) {
        // keep mode runs through ExtractPrims, which harvests USD's
        // diagnostics itself; a second delegate here would double them
        DoPrune(rep, inputPath, options);
    } else {
        // scoped: the harvest has to land in `rep` before it is returned
        detail::DiagnosticsToReport diagnostics(rep);
        DoPrune(rep, inputPath, options);
    }
    return rep;
}

}  // namespace usdprep
