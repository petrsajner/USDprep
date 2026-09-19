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

}  // namespace

Report PruneStage(const std::string& inputPath, const PruneOptions& options) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = options.outputPath;

    const bool keepMode = !options.keepPaths.empty();
    const bool dropMode = !options.dropPaths.empty() || !options.dropTypes.empty() ||
                          !options.dropPurposes.empty();
    if (keepMode == dropMode) {  // both or neither
        rep.Fail("specify exactly one of --except (keep) or --drop");
        return rep;
    }
    if (options.outputPath.empty()) {
        rep.Fail("no output path given");
        return rep;
    }

    if (keepMode) {
        // Keep-only pruning is a masked extraction with several roots.
        ExtractOptions extractOptions;
        extractOptions.primPaths = options.keepPaths;
        extractOptions.outputPath = options.outputPath;
        extractOptions.deinstance = options.deinstance;
        extractOptions.setDefaultPrim = options.setDefaultPrim;
        extractOptions.relinkTextures = options.relinkTextures;
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
        return r;
    }

    // Drop mode: flatten the whole stage first, then delete the selected
    // subtrees from the flattened single layer.
    UsdStageRefPtr stage = UsdStage::Open(inputPath, UsdStage::LoadAll);
    if (!stage) {
        rep.Fail("cannot open stage: " + inputPath);
        return rep;
    }
    rep.before = InspectStage(inputPath).counts;
    const std::string inputDefaultPrim =
        stage->GetDefaultPrim() ? stage->GetDefaultPrim().GetName().GetString() : "";

    // Category filters ("all lights", "everything with purpose guide") are
    // resolved against the input stage and join the explicit drop paths.
    // The two categories are independent requests, so they are resolved
    // separately and unioned — one Select with both filters set would mean
    // "lights that are also guides", which is not what --drop-type
    // --drop-purpose asks for.
    std::vector<std::string> dropPaths = options.dropPaths;
    const auto resolveFilter = [&](const char* label, const std::vector<std::string>& types,
                                   const std::vector<std::string>& purposes) {
        if (types.empty() && purposes.empty()) return true;
        SelectOptions selectOptions;
        selectOptions.types = types;
        selectOptions.purposes = purposes;
        selectOptions.topmostOnly = true;
        const SelectResult sel = SelectPrims(stage, selectOptions);
        if (!sel.error.empty()) {
            rep.Fail(sel.error);
            return false;
        }
        if (sel.paths.empty()) {
            rep.Warn("select", std::string("no prim matched the ") + label +
                                   " filter — nothing dropped by it");
        } else {
            rep.Info("select", std::to_string(sel.paths.size()) +
                                   " subtree(s) matched the " + label + " filter");
        }
        // Prims inside instanced content are only editable once the stage
        // has been de-instanced — otherwise they live in a prototype that
        // this one instance does not own. Leave them alone and say so.
        size_t insideInstances = 0;
        for (const std::string& path : sel.paths) {
            if (!options.deinstance) {
                const UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));
                if (prim && prim.IsInstanceProxy()) {
                    ++insideInstances;
                    continue;
                }
            }
            dropPaths.push_back(path);
        }
        if (insideInstances > 0) {
            rep.Warn("select",
                     std::to_string(insideInstances) +
                         " match(es) live inside instanced content and were kept — "
                         "deleting them requires de-instancing");
        }
        return true;
    };
    if (!resolveFilter("type", options.dropTypes, {})) return rep;
    if (!resolveFilter("purpose", {}, options.dropPurposes)) return rep;

    const std::vector<SdfPath> roots =
        RemoveNestedPaths(ValidatePrimPaths(stage, dropPaths, rep));
    if (!rep.error.empty()) return rep;

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
    rep.Info("flatten", "composition flattened to a single layer");

    // Reopen the flattened file and delete the selected subtrees. After a
    // flatten everything lives in the root layer, so RemovePrim edits the
    // output file only — never the user's input.
    UsdStageRefPtr flat = UsdStage::Open(tmpPath);
    if (!flat) {
        rep.Fail("cannot reopen flattened layer: " + tmpPath);
        return rep;
    }
    for (const SdfPath& p : roots) {
        const UsdPrim prim = flat->GetPrimAtPath(p);
        if (prim && prim.IsInstanceProxy()) {
            rep.Fail("cannot delete " + p.GetAsString() +
                     ": it lives inside instanced content shared by several "
                     "instances — re-run with de-instancing enabled");
            return rep;
        }
        if (!flat->RemovePrim(p)) {
            rep.Fail("failed to delete prim " + p.GetAsString());
            return rep;
        }
    }
    rep.Info("prune-drop",
             std::to_string(roots.size()) + " subtree(s) deleted from the flattened layer");

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

    FinalizeOutput(rep, options.outputPath, tmpPath, options.relinkTextures);
    return rep;
}

}  // namespace usdprep
