// Internal helpers shared by the operation implementations.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/usdzPackage.h>

#include <usdprep/Report.h>
#include <usdprep/StageInfo.h>

namespace usdprep {
namespace detail {

using namespace pxr;

inline bool HasExtension(const std::string& path, const std::string& lowerExt) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == lowerExt;
}

// Intermediate single-file layer; its extension decides the text/binary
// flavor of the flattened output. .usdz is produced from a .usdc temp.
inline std::string TempPathFor(const std::string& outputPath) {
    if (HasExtension(outputPath, ".usda")) return outputPath + ".usdprep-tmp.usda";
    return outputPath + ".usdprep-tmp.usdc";
}

// Author "instanceable = false" opinions into the session layer so the
// flattened export contains plain prims (single-instance instancing buys a
// standalone asset nothing and hides content from simple consumers).
inline size_t DeinstanceStage(const UsdStageRefPtr& stage) {
    stage->SetEditTarget(UsdEditTarget(stage->GetSessionLayer()));
    size_t count = 0;
    for (UsdPrim prim :
         UsdPrimRange(stage->GetPseudoRoot(),
                      UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        if (prim.IsInstanceable() && prim.SetInstanceable(false)) ++count;
    }
    return count;
}

// Author defaultPrim on the flattened output stage, pointing at the
// top-level ancestor of `root`. UsdStage::SetDefaultPrim always writes to
// the stage's root layer — here that is the temporary output file, never
// the user's input. Returns true when a default prim is set afterwards.
inline bool AuthorDefaultPrim(const UsdStageRefPtr& flatStage, const SdfPath& root) {
    if (flatStage->GetDefaultPrim()) return true;
    SdfPath top = root;
    while (top.GetPathElementCount() > 1) top = top.GetParentPath();
    UsdPrim prim = flatStage->GetPrimAtPath(top);
    if (!prim) return false;
    flatStage->SetDefaultPrim(prim);
    return static_cast<bool>(flatStage->GetDefaultPrim());
}

// Shared tail: turn the flattened temp file into the requested output
// (rename, or localize into a .usdz package), then gather after-numbers.
inline void FinalizeOutput(Report& rep, const std::string& outputPath,
                           const std::string& tmpPath) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (HasExtension(outputPath, ".usdz")) {
        if (!UsdUtilsCreateNewUsdzPackage(SdfAssetPath(tmpPath), outputPath)) {
            rep.Fail("usdz packaging failed for " + outputPath);
            return;
        }
        rep.Info("package",
                 "flattened layer and localized dependencies (incl. UDIM "
                 "textures) packaged into " + outputPath);
        fs::remove(tmpPath, ec);
    } else {
        fs::rename(tmpPath, outputPath, ec);
        if (ec) {
            rep.Fail("cannot write output '" + outputPath + "': " + ec.message());
            return;
        }
    }

    std::string err;
    StageInfo info = InspectStage(outputPath, &err);
    if (!err.empty()) {
        rep.Warn("verify", "cannot reopen output for verification: " + err);
    } else {
        rep.after = info.counts;
    }
    rep.outputSizeBytes =
        fs::exists(outputPath, ec) ? fs::file_size(outputPath, ec) : 0;
    rep.ok = true;
}

inline std::vector<SdfPath> ValidatePrimPaths(const UsdStageRefPtr& stage,
                                              const std::vector<std::string>& paths,
                                              Report& rep) {
    std::vector<SdfPath> roots;
    for (const std::string& s : paths) {
        SdfPath p(s);
        if (!p.IsAbsolutePath() || !p.IsPrimPath()) {
            rep.Fail("not an absolute prim path: '" + s + "'");
            return {};
        }
        if (!stage->GetPrimAtPath(p)) {
            rep.Fail("prim does not exist: '" + s + "'");
            return {};
        }
        roots.push_back(p);
    }
    return roots;
}

}  // namespace detail
}  // namespace usdprep
