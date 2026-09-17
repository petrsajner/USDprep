#include <usdprep/Prune.h>

#include <string>
#include <vector>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

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
    const bool dropMode = !options.dropPaths.empty();
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

    const std::vector<SdfPath> roots =
        RemoveNestedPaths(ValidatePrimPaths(stage, options.dropPaths, rep));
    if (!rep.error.empty()) return rep;

    if (options.deinstance) {
        const size_t n = DeinstanceStage(stage);
        if (n > 0) {
            rep.Info("de-instance",
                     std::to_string(n) + " instanceable prim(s) converted to plain prims");
        }
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
        if (!flat->RemovePrim(p)) {
            rep.Fail("failed to delete prim " + p.GetAsString());
            return rep;
        }
    }
    rep.Info("prune-drop",
             std::to_string(roots.size()) + " subtree(s) deleted from the flattened layer");

    if (options.setDefaultPrim && !flat->GetDefaultPrim()) {
        if (UsdPrim firstChild = flat->GetPseudoRoot().GetAllChildren().front()) {
            flat->SetDefaultPrim(firstChild);
            rep.Info("defaultPrim", "set to " + firstChild.GetPrimPath().GetAsString());
        }
    }
    flat->Save();

    FinalizeOutput(rep, options.outputPath, tmpPath);
    return rep;
}

}  // namespace usdprep
