// Internal helpers shared by the operation implementations.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <pxr/usd/usdUtils/localizeAsset.h>
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
//
// It lives in a scratch folder next to the output and carries the output's
// own name, because that file name becomes the root layer name inside a
// .usdz package and ends up in front of whoever opens it.
inline std::string TempDirFor(const std::string& outputPath) {
    namespace fs = std::filesystem;
    const fs::path output(outputPath);
    fs::path dir = output.parent_path();
    if (dir.empty()) dir = ".";
    return (dir / (".usdprep-tmp-" + output.stem().string())).string();
}

inline std::string TempPathFor(const std::string& outputPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir(TempDirFor(outputPath));
    fs::create_directories(dir, ec);
    const std::string name =
        fs::path(outputPath).stem().string() +
        (HasExtension(outputPath, ".usda") ? ".usda" : ".usdc");
    return (dir / name).string();
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

// Flatten turns asset paths into absolute ones by resolving them — but a
// UDIM template ("tex.<UDIM>.exr") never resolves, so it survives the
// export as a relative path and points at nothing as soon as the output
// lands in another folder (the localizer cannot find the tiles either).
// Anchor those paths ourselves, against the layer that authored them.
// Returns how many paths were rewritten.
inline size_t AnchorUnresolvedAssetPaths(const UsdStageRefPtr& stage) {
    stage->SetEditTarget(UsdEditTarget(stage->GetSessionLayer()));
    size_t anchored = 0;
    for (UsdPrim prim :
         UsdPrimRange(stage->GetPseudoRoot(),
                      UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        for (const UsdAttribute& attr : prim.GetAttributes()) {
            if (attr.GetTypeName() != SdfValueTypeNames->Asset) continue;
            SdfAssetPath value;
            if (!attr.Get(&value)) continue;
            const std::string authored = value.GetAssetPath();
            if (authored.empty() || !value.GetResolvedPath().empty()) continue;
            if (!TfIsRelativePath(authored)) continue;
            // The strongest opinion that actually carries a value decides
            // what the path is relative to.
            SdfLayerHandle source;
            for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack()) {
                if (spec && spec->HasField(SdfFieldKeys->Default)) {
                    source = spec->GetLayer();
                    break;
                }
            }
            if (!source) continue;
            std::string absolute = source->ComputeAbsolutePath(authored);
            if (absolute.empty() || absolute == authored) {
                // The resolver refuses to anchor what it cannot resolve, and
                // a UDIM template is exactly that — join the directories
                // ourselves instead.
                const std::string layerDir = TfGetPathName(source->GetRealPath());
                if (layerDir.empty()) continue;
                absolute = TfNormPath(layerDir + authored);
            }
            if (absolute.empty() || absolute == authored ||
                TfIsRelativePath(absolute)) {
                continue;
            }
            if (attr.Set(SdfAssetPath(absolute))) ++anchored;
        }
    }
    return anchored;
}

// Copy every external dependency (textures, including whole UDIM tile
// sets) next to the output and rewrite the asset paths to point there —
// the .usdc/.usda counterpart of what .usdz packaging does. Without this
// a flattened layer still carries absolute paths into the source tree and
// is only "self-contained" on the machine that produced it.
//
// USD's localizer does the copying and the UDIM expansion, but it writes
// the layer next to its dependencies. So we localize into the sidecar
// folder, lift the layer out of it and prefix the now-relative paths.
//
// Returns true when the output file was produced; false means "not needed
// or not possible", and the caller falls back to a plain rename.
inline bool RelinkDependencies(Report& rep, const std::string& outputPath,
                               const std::string& tmpPath) {
    namespace fs = std::filesystem;
    std::error_code ec;

    std::vector<SdfLayerRefPtr> layers;
    std::vector<std::string> assets;
    std::vector<std::string> unresolved;
    UsdUtilsComputeAllDependencies(SdfAssetPath(tmpPath), &layers, &assets, &unresolved);
    if (!unresolved.empty()) {
        rep.Warn("relink", std::to_string(unresolved.size()) +
                               " asset path(s) could not be resolved and stay as "
                               "they are (first: " + unresolved.front() + ")");
    }
    if (assets.empty()) return false;  // nothing external to carry along

    const fs::path output(outputPath);
    fs::path outputDir = output.parent_path();
    if (outputDir.empty()) outputDir = ".";
    const std::string sidecarName = output.stem().string() + "_textures";
    const fs::path sidecarDir = outputDir / sidecarName;

    const bool sidecarExisted = fs::exists(sidecarDir, ec);
    if (!UsdUtilsLocalizeAsset(SdfAssetPath(tmpPath), sidecarDir.string())) {
        rep.Warn("relink",
                 "could not copy the dependencies next to the output; asset paths "
                 "are left pointing at their original location");
        if (!sidecarExisted) fs::remove_all(sidecarDir, ec);
        return false;
    }
    if (sidecarExisted) {
        rep.Warn("relink", "existing folder '" + sidecarName + "' was written into");
    }

    const fs::path localizedLayer = sidecarDir / fs::path(tmpPath).filename();
    fs::remove(output, ec);
    fs::rename(localizedLayer, output, ec);
    if (ec) {
        rep.Warn("relink", "cannot move the relinked layer to '" + outputPath +
                               "': " + ec.message());
        return false;
    }

    // The layer moved one level up, so every path the localizer made
    // relative to the sidecar folder now needs that folder in front of it.
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(outputPath);
    if (!layer) {
        rep.Fail("cannot reopen the relinked layer: " + outputPath);
        return true;  // output exists but is wrong: do not fall back
    }
    size_t rewritten = 0;
    UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) {
        if (assetPath.empty() || fs::path(assetPath).is_absolute()) return assetPath;
        ++rewritten;
        return sidecarName + "/" + assetPath;
    });
    layer->Save();

    size_t copied = 0;
    for (const auto& entry : fs::recursive_directory_iterator(sidecarDir, ec)) {
        if (entry.is_regular_file(ec)) ++copied;
    }
    rep.Info("relink", std::to_string(copied) + " dependency file(s) copied into '" +
                           sidecarName + "' and " + std::to_string(rewritten) +
                           " path(s) rewritten to point there");
    fs::remove(tmpPath, ec);
    return true;
}

// Shared tail: turn the flattened temp file into the requested output
// (rename, or localize into a .usdz package), then gather after-numbers.
inline void FinalizeOutput(Report& rep, const std::string& outputPath,
                           const std::string& tmpPath, bool relinkTextures) {
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
    } else if (!relinkTextures || !RelinkDependencies(rep, outputPath, tmpPath)) {
        if (!rep.error.empty()) return;
        fs::rename(tmpPath, outputPath, ec);
        if (ec) {
            rep.Fail("cannot write output '" + outputPath + "': " + ec.message());
            return;
        }
    }
    if (!rep.error.empty()) return;

    std::string err;
    StageInfo info = InspectStage(outputPath, &err);
    if (!err.empty()) {
        rep.Warn("verify", "cannot reopen output for verification: " + err);
    } else {
        rep.after = info.counts;
    }
    rep.outputSizeBytes =
        fs::exists(outputPath, ec) ? fs::file_size(outputPath, ec) : 0;
    fs::remove_all(TempDirFor(outputPath), ec);
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
