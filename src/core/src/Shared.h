// Internal helpers shared by the operation implementations.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/tf/diagnosticBase.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
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
#include <pxr/usd/usdShade/udimUtils.h>
#include <pxr/usd/usdUtils/coalescingDiagnosticDelegate.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <pxr/usd/usdUtils/localizeAsset.h>
#include <pxr/usd/usdUtils/usdzPackage.h>

#include <usdprep/Report.h>
#include <usdprep/Select.h>
#include <usdprep/StageInfo.h>

namespace usdprep {
namespace detail {

using namespace pxr;

// Everything USD says during a run ends up in the report, grouped, with
// the known noise turned into one plain sentence — instead of scrolling
// past in a console the artist never sees. Lives for the duration of an
// operation; the harvest happens when it goes out of scope, whichever
// way the operation returns.
class DiagnosticsToReport {
public:
    explicit DiagnosticsToReport(Report& rep) : _rep(rep) {}
    ~DiagnosticsToReport() { Harvest(); }
    DiagnosticsToReport(const DiagnosticsToReport&) = delete;
    DiagnosticsToReport& operator=(const DiagnosticsToReport&) = delete;

private:
    void Harvest() {
        std::set<std::string> unknownFields;
        std::vector<std::pair<std::string, size_t>> others;  // message, count
        for (const auto& diagnostic : _delegate.TakeUncoalescedDiagnostics()) {
            const std::string& message = diagnostic->GetCommentary();
            const char* function = diagnostic->GetContext().GetFunction();
            // The localizer complains about every asset path it re-queues
            // after rewriting it, UDIM templates included, and then copies
            // the file anyway. Files that are truly missing were taken out
            // and reported before it ran.
            if (function && std::string(function) == "_EnqueueDependency") continue;
            // Flatten cannot carry metadata fields it has no schema for
            // (a DCC's private bookkeeping, typically). Worth one line, not
            // one warning per prim.
            const size_t unknownAt = message.find("unknown field '");
            if (unknownAt != std::string::npos) {
                const size_t start = unknownAt + 15;
                const size_t end = message.find('\'', start);
                if (end != std::string::npos) unknownFields.insert(message.substr(start, end - start));
                continue;
            }
            if (diagnostic->GetDiagnosticCode() == TfEnum(TF_DIAGNOSTIC_STATUS_TYPE)) continue;
            bool seen = false;
            for (auto& entry : others) {
                if (entry.first == message) {
                    ++entry.second;
                    seen = true;
                    break;
                }
            }
            if (!seen) others.emplace_back(message, 1);
        }

        if (!unknownFields.empty()) {
            std::string names;
            size_t listed = 0;
            for (const std::string& field : unknownFields) {
                if (listed++ == 4) {
                    names += ", ...";
                    break;
                }
                names += (names.empty() ? "" : ", ") + field;
            }
            _rep.Info("metadata", std::to_string(unknownFields.size()) +
                                      " custom metadata field(s) from the source pipeline were "
                                      "not carried over: " + names);
        }
        constexpr size_t kMaxListed = 8;
        for (size_t i = 0; i < others.size() && i < kMaxListed; ++i) {
            _rep.Warn("usd", others[i].first +
                                 (others[i].second > 1 ? " (x" + std::to_string(others[i].second) + ")"
                                                       : ""));
        }
        if (others.size() > kMaxListed) {
            _rep.Warn("usd", "... and " + std::to_string(others.size() - kMaxListed) +
                                 " more kind(s) of warning");
        }
    }

    Report& _rep;
    UsdUtilsCoalescingDiagnosticDelegate _delegate;
};

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

// Remove whole categories (every light, everything with purpose guide)
// from an already flattened stage. The categories are independent
// requests, so they are resolved one at a time and unioned: one Select
// with both filters set would mean "lights that are also guides".
inline void DropCategoriesFromStage(Report& rep, const UsdStageRefPtr& flat,
                                    const std::vector<std::string>& types,
                                    const std::vector<std::string>& purposes) {
    if (types.empty() && purposes.empty()) return;

    std::vector<std::string> paths;
    size_t insideInstances = 0;
    const auto collect = [&](const char* label, const std::vector<std::string>& t,
                             const std::vector<std::string>& p) {
        if (t.empty() && p.empty()) return;
        SelectOptions options;
        options.types = t;
        options.purposes = p;
        options.topmostOnly = true;
        const SelectResult sel = SelectPrims(flat, options);
        if (!sel.error.empty()) {
            rep.Warn("strip", sel.error);
            return;
        }
        if (sel.paths.empty()) {
            rep.Warn("strip", std::string("no prim matched the ") + label +
                                  " filter — nothing dropped by it");
            return;
        }
        for (const std::string& path : sel.paths) {
            const UsdPrim prim = flat->GetPrimAtPath(SdfPath(path));
            // Content inside an instance belongs to a prototype shared by
            // every instance, so it cannot be dropped for just this one.
            if (prim && prim.IsInstanceProxy()) {
                ++insideInstances;
                continue;
            }
            paths.push_back(path);
        }
    };
    collect("type", types, {});
    collect("purpose", {}, purposes);

    size_t removed = 0;
    for (const std::string& path : paths) {
        const SdfPath primPath(path);
        // A match from the other category may already have taken this
        // subtree with it.
        if (!flat->GetPrimAtPath(primPath)) continue;
        if (flat->RemovePrim(primPath)) ++removed;
    }
    if (removed > 0) {
        rep.Info("strip", std::to_string(removed) +
                              " subtree(s) removed by the type/purpose filter");
    }
    if (insideInstances > 0) {
        rep.Warn("strip", std::to_string(insideInstances) +
                              " match(es) live inside instanced content and were kept "
                              "— deleting them requires de-instancing");
    }
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

// A texture that is not on this machine must not sink the whole export:
// USD's packager and localizer both give up on the first dependency they
// cannot find. Such references are dead anyway, so they are taken out of
// the flattened layer and named in the report, and the export goes on
// with everything that does exist.
inline void DropMissingDependencies(Report& rep, const std::string& tmpPath) {
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(tmpPath);
    if (!layer) return;
    std::vector<std::string> missing;
    UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) -> std::string {
        if (assetPath.empty()) return assetPath;
        bool exists;
        if (UsdShadeUdimUtils::IsUdimIdentifier(assetPath)) {
            exists = !UsdShadeUdimUtils::ResolveUdimTilePaths(assetPath, layer).empty();
        } else {
            exists = !ArGetResolver()
                          .Resolve(SdfComputeAssetPathRelativeToLayer(layer, assetPath))
                          .IsEmpty();
        }
        if (exists) return assetPath;
        missing.push_back(assetPath);
        return std::string();
    });
    if (missing.empty()) return;
    layer->Save();
    std::string names;
    for (size_t i = 0; i < missing.size() && i < 3; ++i) {
        names += (i == 0 ? "" : ", ") + std::filesystem::path(missing[i]).filename().string();
    }
    if (missing.size() > 3) names += ", ...";
    rep.Warn("textures", std::to_string(missing.size()) +
                             " texture file(s) are not on this machine and were left out: " +
                             names);
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

    DropMissingDependencies(rep, tmpPath);

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
