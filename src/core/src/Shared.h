// Internal helpers shared by the operation implementations.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec3i.h>
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
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
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
// When instancing is kept, everything instanced lives in the prototypes of
// the flattened layer: top-level "over" prims named Flattened_Prototype_N
// that the instances reference. A stage traversal does not visit an over,
// and an instance proxy cannot be edited - so no clean-up would ever reach
// instanced content (measured on ALab: the export came out larger than the
// de-instanced one, hero textures, proxies and lights all still in it).
// For the time of the post-pass the prototypes are turned into ordinary
// defined prims: every pass then treats them like any other subtree, once
// per prototype instead of once per instance. Restore() turns them back.
class ExposedPrototypes {
public:
    explicit ExposedPrototypes(const UsdStageRefPtr& flat) : _layer(flat->GetRootLayer()) {
        for (const SdfPrimSpecHandle& spec : _layer->GetRootPrims()) {
            if (spec->GetSpecifier() == SdfSpecifierOver && IsPrototypeName(spec->GetName())) {
                spec->SetSpecifier(SdfSpecifierDef);
                _paths.push_back(spec->GetPath());
            }
        }
    }
    ~ExposedPrototypes() { Restore(); }
    void Restore() {
        for (const SdfPath& path : _paths) {
            if (const SdfPrimSpecHandle spec = _layer->GetPrimAtPath(path)) spec->SetSpecifier(SdfSpecifierOver);
        }
        _paths.clear();
    }
    size_t Count() const { return _paths.size(); }
    static bool IsPrototypeName(const std::string& name) { return name.rfind("Flattened_Prototype", 0) == 0; }
    // True while some prototype of this stage is exposed.
    static bool ActiveOn(const UsdStageRefPtr& flat) {
        for (const UsdPrim& prim : flat->GetPseudoRoot().GetChildren()) {
            if (IsPrototypeName(prim.GetName().GetString())) return true;  // GetChildren() lists defined prims only
        }
        return false;
    }

private:
    SdfLayerHandle _layer;
    std::vector<SdfPath> _paths;
};

// A .usdc saved over itself appends: the arrays it held stay in the file, so
// a mesh decimated to a hundredth or a subtree pruned away does not make the
// file any smaller. The post-passes therefore write the layer anew next to
// it; SwapInPacked() puts that in place once the stage is closed (Windows
// does not replace a file that is still mapped).
inline std::string PackedPathFor(const std::string& tmpPath) { return tmpPath + ".packed.usdc"; }

inline bool SaveCompact(const UsdStageRefPtr& flat, const std::string& tmpPath) {
    if (!HasExtension(tmpPath, ".usdc")) {
        flat->Save();  // text is written whole anyway
        return true;
    }
    return flat->GetRootLayer()->Export(PackedPathFor(tmpPath));
}

inline void SwapInPacked(const std::string& tmpPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string packed = PackedPathFor(tmpPath);
    if (!fs::exists(packed, ec)) return;
    fs::remove(tmpPath, ec);
    fs::rename(packed, tmpPath, ec);
    if (ec) fs::remove(packed, ec);  // the appended file stays: bigger, but complete
}

// Materials the selected objects use that live elsewhere in the scene (a
// Looks or Materials scope beside the geometry, as in an OBJ turned into
// USD): without them the objects come out grey. Bound directly or
// inherited, for every purpose, with the shader nodes their networks reach
// outside the material.
inline std::vector<SdfPath> MaterialsFromOutside(const UsdStageRefPtr& stage, const std::vector<SdfPath>& roots) {
    const auto inside = [&](const SdfPath& path) {
        for (const SdfPath& root : roots) {
            if (path.HasPrefix(root)) return true;
        }
        return false;
    };
    std::set<SdfPath> found;
    std::vector<SdfPath> pending;
    // The binding relationships are read as they are written - on the prim or
    // an ancestor, the nearest one per purpose - rather than through
    // ComputeBoundMaterial, which warns about every older file that binds
    // without applying MaterialBindingAPI.
    const TfToken relationships[] = {TfToken("material:binding"), TfToken("material:binding:preview"),
                                     TfToken("material:binding:full")};
    for (const SdfPath& root : roots) {
        const UsdPrim top = stage->GetPrimAtPath(root);
        if (!top) continue;
        for (const UsdPrim& prim : UsdPrimRange(top, UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            if (!prim.IsA<UsdGeomGprim>() && !prim.IsA<UsdGeomSubset>()) continue;
            for (const TfToken& name : relationships) {
                for (UsdPrim at = prim; at && !at.IsPseudoRoot(); at = at.GetParent()) {
                    const UsdRelationship binding = at.GetRelationship(name);
                    SdfPathVector targets;
                    if (!binding || !binding.GetForwardedTargets(&targets) || targets.empty()) continue;
                    const SdfPath path = targets.front().GetPrimPath();
                    if (!inside(path) && stage->GetPrimAtPath(path) && found.insert(path).second) {
                        pending.push_back(path);
                    }
                    break;  // the nearest binding wins
                }
            }
        }
    }
    // shader networks may reach out of the material (a shared node graph)
    const auto covered = [&](const SdfPath& path) {
        if (inside(path)) return true;
        for (const SdfPath& known : found) {
            if (path.HasPrefix(known)) return true;
        }
        return false;
    };
    while (!pending.empty()) {
        const SdfPath next = pending.back();
        pending.pop_back();
        const UsdPrim prim = stage->GetPrimAtPath(next);
        if (!prim) continue;
        for (const UsdPrim& node : UsdPrimRange(prim)) {
            const UsdShadeConnectableAPI connectable(node);
            if (!connectable) continue;
            std::vector<UsdShadeInput> inputs = connectable.GetInputs();
            std::vector<UsdShadeOutput> outputs = connectable.GetOutputs();
            std::vector<UsdAttribute> attributes;
            for (const UsdShadeInput& input : inputs) attributes.push_back(input.GetAttr());
            for (const UsdShadeOutput& output : outputs) attributes.push_back(output.GetAttr());
            for (const UsdAttribute& attribute : attributes) {
                SdfPathVector sources;
                attribute.GetConnections(&sources);
                for (const SdfPath& source : sources) {
                    const SdfPath owner = source.GetPrimPath();
                    if (!covered(owner) && found.insert(owner).second) pending.push_back(owner);
                }
            }
        }
    }
    return std::vector<SdfPath>(found.begin(), found.end());
}

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
            // not a warning: the presets ask for lights and proxies to go
            // whether a scene has any or not
            rep.Info("strip", std::string("no prim matched the ") + label +
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
    // with the prototypes exposed the same prims were matched, and removed, there
    if (insideInstances > 0 && !ExposedPrototypes::ActiveOn(flat)) {
        rep.Warn("strip", std::to_string(insideInstances) +
                              " match(es) live inside instanced content and were kept "
                              "— deleting them requires de-instancing");
    }
}

// The material diet, on an already flattened stage:
//
// 1. Where a prim binds one material for "full" renders and another for
//    "preview" (the production pattern: hero UDIM sets vs. small JPGs),
//    keep the one asked for and bind it for every purpose, so whatever
//    Nuke asks for it gets the light one. `purpose` is "preview", "full"
//    or "all" (leave both).
// 2. Material outputs for a specific render context (outputs:arnold:*,
//    outputs:ri:*, outputs:mtlx:*) are dropped, and with them every shader
//    only they reached. The universal surface/displacement/volume stay.
// 3. Materials nothing binds any more are removed.
//
// Textures only the removed shaders referenced disappear with them, which
// is where the weight goes.
inline void StripMaterials(Report& rep, const UsdStageRefPtr& flat, const std::string& purpose,
                           bool stripRenderContexts, bool stripUnused, bool udimAtlas) {
    const bool choose = purpose == "preview" || purpose == "full";
    if (!choose && !stripRenderContexts && !stripUnused) return;
    const auto shown = UsdTraverseInstanceProxies(UsdPrimDefaultPredicate);

    // A material Nuke would render black: one of its textures is a UDIM
    // set of more than one tile, and Nuke does not expand the template.
    std::map<SdfPath, bool> unreadableCache;
    const auto unreadable = [&](const UsdPrim& materialPrim) {
        const auto cached = unreadableCache.find(materialPrim.GetPath());
        if (cached != unreadableCache.end()) return cached->second;
        bool result = false;
        for (const UsdPrim& node : UsdPrimRange(materialPrim, shown)) {
            for (const UsdAttribute& attr : node.GetAttributes()) {
                if (attr.GetTypeName() != SdfValueTypeNames->Asset) continue;
                SdfAssetPath asset;
                if (!attr.Get(&asset) || !UsdShadeUdimUtils::IsUdimIdentifier(asset.GetAssetPath())) continue;
                // a set a UsdUVTexture reads is about to become an atlas
                TfToken id;
                if (udimAtlas && UsdShadeShader(node).GetShaderId(&id) && id == "UsdUVTexture") continue;
                if (UsdShadeUdimUtils::ResolveUdimTilePaths(asset.GetAssetPath(), flat->GetRootLayer()).size() > 1) {
                    result = true;
                }
            }
        }
        unreadableCache[materialPrim.GetPath()] = result;
        return result;
    };

    // ----- 1. one material per prim, the purpose that was asked for -----
    size_t rebound = 0;
    size_t fellBack = 0;
    if (choose) {
        const TfToken wanted(purpose);
        const TfToken other = purpose == "preview" ? UsdShadeTokens->full : UsdShadeTokens->preview;
        for (UsdPrim prim : UsdPrimRange(flat->GetPseudoRoot(), shown)) {
            if (prim.IsPseudoRoot() || prim.IsInstanceProxy()) continue;
            UsdShadeMaterialBindingAPI binding(prim);
            const UsdRelationship wantedRel = binding.GetDirectBindingRel(wanted);
            const UsdRelationship otherRel = binding.GetDirectBindingRel(other);
            if (!wantedRel && !otherRel) continue;
            SdfPathVector targets;
            if (wantedRel) wantedRel.GetTargets(&targets);
            // nothing for the wanted purpose: the other one is all there is
            if (targets.empty() && otherRel) otherRel.GetTargets(&targets);
            UsdShadeMaterial material(
                targets.empty() ? UsdPrim() : flat->GetPrimAtPath(targets.front()));
            if (!material) continue;
            // The hero material would be black in Nuke and there is a
            // light one next to it: the light one it is.
            if (purpose == "full" && otherRel && unreadable(material.GetPrim())) {
                SdfPathVector lightTargets;
                otherRel.GetTargets(&lightTargets);
                const UsdShadeMaterial light(
                    lightTargets.empty() ? UsdPrim() : flat->GetPrimAtPath(lightTargets.front()));
                if (light && !unreadable(light.GetPrim())) {
                    material = light;
                    ++fellBack;
                }
            }
            UsdShadeMaterialBindingAPI::Apply(prim);
            UsdShadeMaterialBindingAPI(prim).Bind(material, UsdShadeTokens->fallbackStrength,
                                                  UsdShadeTokens->allPurpose);
            if (wantedRel) prim.RemoveProperty(wantedRel.GetName());
            if (otherRel) prim.RemoveProperty(otherRel.GetName());
            ++rebound;
        }
    }

    // ----- 2. render-context outputs and the shaders only they reach ---
    size_t contextsRemoved = 0;
    size_t shadersRemoved = 0;
    std::vector<UsdPrim> materials;
    for (const UsdPrim& prim : UsdPrimRange(flat->GetPseudoRoot(), shown)) {
        if (!prim.IsInstanceProxy() && prim.IsA<UsdShadeMaterial>()) materials.push_back(prim);
    }
    if (stripRenderContexts) {
        for (UsdPrim materialPrim : materials) {
            std::vector<UsdAttribute> outputs;
            for (const UsdAttribute& attr : materialPrim.GetAttributes()) {
                const std::string name = attr.GetName().GetString();
                if (name.rfind("outputs:", 0) != 0) continue;
                const std::string terminal = name.substr(name.rfind(':') + 1);
                if (terminal != "surface" && terminal != "displacement" && terminal != "volume") continue;
                const bool universal = name == "outputs:" + terminal;
                if (universal) continue;
                outputs.push_back(attr);
            }
            for (const UsdAttribute& attr : outputs) {
                if (materialPrim.RemoveProperty(attr.GetName())) ++contextsRemoved;
            }

            // What the remaining outputs still reach, following connections.
            std::set<SdfPath> reachable;
            std::vector<SdfPath> frontier;
            for (const UsdAttribute& attr : materialPrim.GetAttributes()) {
                SdfPathVector sources;
                if (attr.GetConnections(&sources)) {
                    for (const SdfPath& s : sources) frontier.push_back(s.GetPrimPath());
                }
            }
            while (!frontier.empty()) {
                const SdfPath path = frontier.back();
                frontier.pop_back();
                if (!reachable.insert(path).second) continue;
                const UsdPrim node = flat->GetPrimAtPath(path);
                if (!node) continue;
                for (const UsdAttribute& attr : node.GetAttributes()) {
                    SdfPathVector sources;
                    if (attr.GetConnections(&sources)) {
                        for (const SdfPath& s : sources) frontier.push_back(s.GetPrimPath());
                    }
                }
            }
            std::vector<SdfPath> orphans;
            for (const UsdPrim& node : UsdPrimRange(materialPrim, shown)) {
                if (node == materialPrim || node.IsInstanceProxy()) continue;
                const TfToken type = node.GetTypeName();
                if (type != "Shader" && type != "NodeGraph") continue;
                if (!reachable.count(node.GetPath())) orphans.push_back(node.GetPath());
            }
            // deepest first, so a removed subtree is not visited again
            std::sort(orphans.begin(), orphans.end(),
                      [](const SdfPath& a, const SdfPath& b) { return a.GetPathElementCount() > b.GetPathElementCount(); });
            for (const SdfPath& path : orphans) {
                if (flat->GetPrimAtPath(path) && flat->RemovePrim(path)) ++shadersRemoved;
            }
        }
    }

    // ----- 3. materials nothing binds --------------------------------------
    size_t materialsRemoved = 0;
    if (stripUnused) {
        std::set<SdfPath> bound;
        for (const UsdPrim& prim : UsdPrimRange(flat->GetPseudoRoot(), shown)) {
            for (const UsdRelationship& rel : prim.GetRelationships()) {
                if (rel.GetName().GetString().rfind("material:binding", 0) != 0) continue;
                SdfPathVector targets;
                rel.GetTargets(&targets);
                for (const SdfPath& t : targets) bound.insert(t);
            }
        }
        for (const UsdPrim& materialPrim : materials) {
            const SdfPath path = materialPrim.GetPath();
            if (bound.count(path)) continue;
            // a material bound from inside another material's subtree is
            // still a material; anything that targets it keeps it
            if (flat->GetPrimAtPath(path) && flat->RemovePrim(path)) ++materialsRemoved;
        }
    }

    if (rebound > 0) {
        rep.Info("materials", std::to_string(rebound) + " binding(s) switched to the " + purpose +
                                  " material; the " +
                                  (purpose == "preview" ? "full-quality" : "preview") +
                                  " one is no longer used");
    }
    if (fellBack > 0) {
        rep.Warn("nuke", std::to_string(fellBack) +
                             " object(s) got their light material instead: the full-quality one uses "
                             "multi-tile UDIM textures, which Nuke 17 does not read");
    }
    if (contextsRemoved > 0) {
        rep.Info("materials", std::to_string(contextsRemoved) +
                                  " renderer-specific material output(s) removed");
    }
    if (shadersRemoved > 0) {
        rep.Info("materials", std::to_string(shadersRemoved) +
                                  " shader(s) nothing connects to any more removed");
    }
    if (materialsRemoved > 0) {
        rep.Info("materials", std::to_string(materialsRemoved) + " unused material(s) removed");
    }
}

// Draw-mode cards: a viewer's stand-in for an asset (six textures on a
// box) that Nuke never draws, yet whose textures travel with every
// export. The whole UsdGeomModelAPI draw-mode setup goes; the geometry
// is right there.
inline void StripDrawModeCards(Report& rep, const UsdStageRefPtr& flat) {
    static const char* kDrawModeAttributes[] = {
        "model:applyDrawMode",   "model:drawMode",        "model:drawModeColor",
        "model:cardGeometry",    "model:cardTextureXPos", "model:cardTextureXNeg",
        "model:cardTextureYPos", "model:cardTextureYNeg", "model:cardTextureZPos",
        "model:cardTextureZNeg",
    };
    size_t prims = 0;
    size_t textures = 0;
    for (UsdPrim prim :
         UsdPrimRange(flat->GetPseudoRoot(), UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        if (prim.IsPseudoRoot() || prim.IsInstanceProxy()) continue;
        bool touched = false;
        for (const char* name : kDrawModeAttributes) {
            const TfToken token(name);
            const UsdAttribute attr = prim.GetAttribute(token);
            if (!attr || !attr.HasAuthoredValue()) continue;
            SdfAssetPath asset;
            if (attr.GetTypeName() == SdfValueTypeNames->Asset && attr.Get(&asset) &&
                !asset.GetAssetPath().empty()) {
                ++textures;
            }
            if (prim.RemoveProperty(token)) touched = true;
        }
        if (touched) ++prims;
    }
    if (prims > 0) {
        rep.Info("cards", "preview-card setup removed from " + std::to_string(prims) +
                              " object(s); " + std::to_string(textures) +
                              " card texture(s) no longer needed");
    }
}

// NukeCompat.cpp: what is left after the recipe, made readable for Nuke -
// converted or replaced, never dropped, and every substitution reported.
void MakeNukeReadable(Report& rep, const UsdStageRefPtr& flat);

// An imported scene (Import.h) carries the path of the file it was made
// from. The files made from it are not that file: the note stays behind.
inline bool HasImportSource(const UsdStageRefPtr& stage) {
    return stage && stage->GetRootLayer()->GetCustomLayerData().count("usdprep:importedFrom") > 0;
}
inline void ForgetImportSource(const UsdStageRefPtr& flat) {
    VtDictionary data = flat->GetRootLayer()->GetCustomLayerData();
    if (data.erase("usdprep:importedFrom") == 0) return;
    if (data.empty()) {
        flat->GetRootLayer()->ClearCustomLayerData();
    } else {
        flat->GetRootLayer()->SetCustomLayerData(data);
    }
}

// UdimAtlas.cpp: stitch every multi-tile UDIM set a UsdUVTexture reads
// into one image under `atlasDir` and put a UsdTransform2d in front of
// the texture. `maxTileSize` caps each tile (0 = as it is).
void AtlasUdimTextures(Report& rep, const UsdStageRefPtr& flat, const std::string& atlasDir,
                       int maxTileSize);

// RawTextures.cpp: Nuke decodes every 8-bit texture as sRGB; 8-bit textures
// a UsdUVTexture reads as "raw" get a pre-compensated copy under `folder`.
void CompensateRawTextures(Report& rep, const UsdStageRefPtr& flat, const std::string& folder);

// Where the atlases go: the scratch folder when the textures get copied
// next to the output anyway, else straight into "<name>_textures".
inline std::string AtlasDirFor(const std::string& outputPath, const std::string& tmpPath,
                               bool relinkTextures) {
    namespace fs = std::filesystem;
    if (relinkTextures) return (fs::path(tmpPath).parent_path() / "textures" / "atlas").string();
    const fs::path out(outputPath);
    return (out.parent_path() / (out.stem().string() + "_textures")).string();
}

// Simplify.cpp: decimate every mesh with at least `minFaces` faces to
// `ratio` of its triangles (0 < ratio < 1; anything else = leave alone).
void SimplifyMeshes(Report& rep, const UsdStageRefPtr& flat, double ratio, size_t minFaces = 500);

inline std::string FormatFrame(double frame) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", frame);
    return buffer;
}

// Animation on a diet, on the flattened output. `mode` is "all" (leave
// it), "range" (time samples outside [start, end] go — a simulation's
// pre-roll, typically — keeping one bracketing sample on each side so the
// boundary frames still interpolate) or "static" (one frame becomes the
// value, no samples remain). A NaN frame means "the stage's own".
inline void TrimAnimation(Report& rep, const UsdStageRefPtr& flat, const std::string& mode,
                          double frameStart, double frameEnd, double staticFrame) {
    if (mode != "range" && mode != "static") return;
    const SdfLayerHandle layer = flat->GetRootLayer();
    double start = std::isnan(frameStart) ? flat->GetStartTimeCode() : frameStart;
    double end = std::isnan(frameEnd) ? flat->GetEndTimeCode() : frameEnd;
    if (end < start) std::swap(start, end);

    // Gather first: editing the layer while traversing it is asking for
    // trouble.
    std::vector<SdfPath> animated;
    layer->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& path) {
        if (path.IsPropertyPath() && layer->GetNumTimeSamplesForPath(path) > 0) {
            animated.push_back(path);
        }
    });
    if (animated.empty()) return;

    if (mode == "static") {
        const double frame = std::isnan(staticFrame) ? start : staticFrame;
        size_t baked = 0;
        for (const SdfPath& path : animated) {
            const UsdAttribute attr = flat->GetAttributeAtPath(path);
            if (!attr) continue;
            VtValue value;
            if (!attr.Get(&value, UsdTimeCode(frame))) continue;  // interpolated where needed
            attr.Clear();  // the default and every sample
            if (attr.Set(value)) ++baked;
        }
        flat->SetStartTimeCode(frame);
        flat->SetEndTimeCode(frame);
        rep.Info("trim", "frame " + FormatFrame(frame) + " baked as the only value of " +
                             std::to_string(baked) + " animated attribute(s); the animation is gone");
        return;
    }

    size_t erased = 0;
    size_t attributes = 0;
    for (const SdfPath& path : animated) {
        const std::set<double> samples = layer->ListTimeSamplesForPath(path);
        double before = -std::numeric_limits<double>::infinity();
        double after = std::numeric_limits<double>::infinity();
        for (const double t : samples) {
            if (t < start) before = t;                 // the last one before the range
            if (t > end && after == std::numeric_limits<double>::infinity()) after = t;
        }
        bool touched = false;
        for (const double t : samples) {
            if (t >= start && t <= end) continue;
            if (t == before || t == after) continue;   // bracketing: kept
            layer->EraseTimeSample(path, t);
            ++erased;
            touched = true;
        }
        if (touched) ++attributes;
    }
    if (!std::isnan(frameStart) || !std::isnan(frameEnd)) {
        flat->SetStartTimeCode(start);
        flat->SetEndTimeCode(end);
    }
    if (erased > 0) {
        rep.Info("trim", std::to_string(erased) + " time sample(s) outside frames " +
                             FormatFrame(start) + "-" + FormatFrame(end) + " removed from " +
                             std::to_string(attributes) + " attribute(s)");
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

// A texture bigger than Nuke will ever show (hero 8K UDIM sets) is scaled
// down to `maxSize` on its longer side — into a copy under the scratch
// folder, which the packager or the localizer then picks up instead of
// the original. The original file is never touched. USD's own Hio reads
// and writes PNG, JPEG and EXR; anything it cannot read is left as it is
// and named in the report.
inline void CapTextures(Report& rep, const std::string& tmpPath, int maxSize) {
    if (maxSize <= 0) return;
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(tmpPath);
    if (!layer) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path scratch = fs::path(tmpPath).parent_path() / "textures";

    size_t scaled = 0;
    int largest = 0;
    std::vector<std::string> untouched;         // formats this build cannot read
    std::map<std::string, std::string> copies;  // source -> capped copy ("" = left as is)

    // Copies keep their file name (a UDIM set has to stay a set) and go
    // into a folder named after their source folder, so two sets that
    // happen to share names do not collide.
    const auto copyFolderFor = [&](const std::string& source) {
        const std::string parent = fs::path(source).parent_path().string();
        char name[24];
        std::snprintf(name, sizeof(name), "%08zx", std::hash<std::string>{}(parent) & 0xffffffffu);
        return scratch / name;
    };

    const auto capFile = [&](const std::string& source) -> std::string {
        const auto known = copies.find(source);
        if (known != copies.end()) return known->second;
        copies[source] = "";
        // an atlas was capped tile by tile when it was built
        if (fs::path(source).filename().string().find(".atlas.") != std::string::npos) return "";
        const HioImageSharedPtr image = HioImage::OpenForReading(
            source, 0, 0, HioImage::SourceColorSpace::Raw, /*suppressErrors=*/true);
        if (!image) {
            untouched.push_back(source);
            return "";
        }
        const int width = image->GetWidth();
        const int height = image->GetHeight();
        if (std::max(width, height) <= maxSize) return "";
        const HioFormat format = image->GetFormat();
        const HioType type = HioGetHioType(format);
        const int channels = HioGetComponentCount(format);
        if (type != HioTypeUnsignedByte && type != HioTypeUnsignedByteSRGB &&
            type != HioTypeHalfFloat && type != HioTypeFloat) {
            untouched.push_back(source);
            return "";
        }
        std::vector<unsigned char> src(HioGetDataSize(format, GfVec3i(width, height, 1)));
        HioImage::StorageSpec in;
        in.width = width;
        in.height = height;
        in.depth = 1;
        in.format = format;
        in.data = src.data();
        if (!image->Read(in)) {
            untouched.push_back(source);
            return "";
        }

        const double factor = static_cast<double>(maxSize) / std::max(width, height);
        const int newWidth = std::max(1, static_cast<int>(std::lround(width * factor)));
        const int newHeight = std::max(1, static_cast<int>(std::lround(height * factor)));
        std::vector<unsigned char> dst(HioGetDataSize(format, GfVec3i(newWidth, newHeight, 1)));
        const auto load = [&](size_t index) -> float {
            switch (type) {
                case HioTypeHalfFloat: return static_cast<float>(reinterpret_cast<const GfHalf*>(src.data())[index]);
                case HioTypeFloat: return reinterpret_cast<const float*>(src.data())[index];
                default: return src[index] / 255.0f;
            }
        };
        const auto store = [&](size_t index, float value) {
            switch (type) {
                case HioTypeHalfFloat: reinterpret_cast<GfHalf*>(dst.data())[index] = GfHalf(value); break;
                case HioTypeFloat: reinterpret_cast<float*>(dst.data())[index] = value; break;
                default: dst[index] = static_cast<unsigned char>(std::lround(std::min(std::max(value, 0.0f), 1.0f) * 255.0f)); break;
            }
        };
        // Area average: every output pixel is the mean of the source box
        // it covers. Not fancy, but it never rings and never aliases.
        for (int y = 0; y < newHeight; ++y) {
            const int y0 = y * height / newHeight;
            const int y1 = std::max(y0 + 1, (y + 1) * height / newHeight);
            for (int x = 0; x < newWidth; ++x) {
                const int x0 = x * width / newWidth;
                const int x1 = std::max(x0 + 1, (x + 1) * width / newWidth);
                const float count = static_cast<float>((y1 - y0) * (x1 - x0));
                for (int c = 0; c < channels; ++c) {
                    float sum = 0.0f;
                    for (int sy = y0; sy < y1; ++sy) {
                        for (int sx = x0; sx < x1; ++sx) {
                            sum += load((static_cast<size_t>(sy) * width + sx) * channels + c);
                        }
                    }
                    store((static_cast<size_t>(y) * newWidth + x) * channels + c, sum / count);
                }
            }
        }

        const fs::path folder = copyFolderFor(source);
        fs::create_directories(folder, ec);
        const fs::path copy = folder / fs::path(source).filename();
        const HioImageSharedPtr out = HioImage::OpenForWriting(copy.string());
        HioImage::StorageSpec spec;
        spec.width = newWidth;
        spec.height = newHeight;
        spec.depth = 1;
        spec.format = format;
        spec.data = dst.data();
        if (!out || !out->Write(spec)) {
            untouched.push_back(source);
            return "";
        }
        ++scaled;
        largest = std::max(largest, std::max(width, height));
        copies[source] = copy.string();
        return copy.string();
    };

    UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) -> std::string {
        if (assetPath.empty()) return assetPath;
        if (UsdShadeUdimUtils::IsUdimIdentifier(assetPath)) {
            const auto tiles = UsdShadeUdimUtils::ResolveUdimTilePaths(assetPath, layer);
            if (tiles.empty()) return assetPath;
            bool any = false;
            for (const auto& tile : tiles) {
                if (!capFile(tile.first).empty()) any = true;
            }
            if (!any) return assetPath;
            // the set stays whole: tiles under the cap are copied as they are
            const fs::path folder = copyFolderFor(tiles.front().first);
            for (const auto& tile : tiles) {
                if (copies[tile.first].empty()) {
                    fs::copy_file(tile.first, folder / fs::path(tile.first).filename(),
                                  fs::copy_options::overwrite_existing, ec);
                }
            }
            return (folder / fs::path(assetPath).filename()).string();
        }
        const std::string resolved =
            ArGetResolver().Resolve(SdfComputeAssetPathRelativeToLayer(layer, assetPath)).GetPathString();
        if (resolved.empty()) return assetPath;
        const std::string copy = capFile(resolved);
        return copy.empty() ? assetPath : copy;
    });
    layer->Save();

    if (scaled > 0) {
        rep.Info("textures", std::to_string(scaled) + " texture file(s) larger than " +
                                 std::to_string(maxSize) + " px scaled down (largest was " +
                                 std::to_string(largest) + " px)");
    }
    if (!untouched.empty()) {
        std::string names;
        for (size_t i = 0; i < untouched.size() && i < 3; ++i) {
            names += (i == 0 ? "" : ", ") + fs::path(untouched[i]).filename().string();
        }
        if (untouched.size() > 3) names += ", ...";
        rep.Warn("textures", std::to_string(untouched.size()) +
                                 " texture file(s) in a format this build cannot read were left "
                                 "as they are: " + names);
    }
}

// What Nuke 17 will and will not read, learned by rendering there: it
// does not expand a "<UDIM>" template (the material renders black), and
// it does not read textures from inside a .usdz package. A set with a
// single tile is rewritten to that tile, which Nuke reads fine; the rest
// is said out loud in the report so nobody hunts for a black material.
inline void NukeReadability(Report& rep, const std::string& tmpPath, const std::string& outputPath) {
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(tmpPath);
    if (!layer) return;
    size_t collapsed = 0;
    size_t multiTile = 0;
    UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) -> std::string {
        if (assetPath.empty() || !UsdShadeUdimUtils::IsUdimIdentifier(assetPath)) return assetPath;
        const auto tiles = UsdShadeUdimUtils::ResolveUdimTilePaths(assetPath, layer);
        if (tiles.size() == 1) {
            ++collapsed;
            return tiles.front().first;
        }
        if (tiles.size() > 1) ++multiTile;
        return assetPath;
    });
    if (collapsed > 0) {
        layer->Save();
        rep.Info("textures", std::to_string(collapsed) +
                                 " single-tile UDIM set(s) rewritten to the tile itself (Nuke does not "
                                 "read UDIM templates)");
    }
    if (multiTile > 0) {
        rep.Warn("nuke", std::to_string(multiTile) +
                             " texture(s) are multi-tile UDIM sets, which Nuke 17 does not read - those "
                             "materials render black there (the preview materials avoid this)");
    }
    if (HasExtension(outputPath, ".usdz")) {
        rep.Warn("nuke", "Nuke 17 loads the geometry of a .usdz but not the textures inside it; "
                         "export a .usdc with its textures folder for Nuke");
    }
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
                               const std::string& tmpPath, const std::string& sidecarStem) {
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
    const std::string sidecarName = sidecarStem + "_textures";
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

// MeshExport.cpp: the prepared .usdc written out as an .obj or an .abc
// (plus a .nk that wires the textures in) for Nuke's classic 3D. `frame`
// picks the still; NaN = the scene's start for an .obj, the whole range
// for an .abc.
bool ExportMeshFile(Report& rep, const std::string& usdPath, const std::string& outPath, double frame);

// Shared tail: turn the flattened temp file into the requested output
// (rename, or localize into a .usdz package), then gather after-numbers.
// An .obj / .abc output is made from the finished .usdc, which is then removed.
inline void FinalizeOutput(Report& rep, const std::string& requestedPath,
                           const std::string& tmpPath, bool relinkTextures,
                           int maxTextureSize, double frame) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool asObj = HasExtension(requestedPath, ".obj") || HasExtension(requestedPath, ".abc");
    const fs::path requested(requestedPath);
    const std::string outputPath =
        asObj ? (requested.parent_path() / (requested.stem().string() + ".usdprep-source.usdc")).string()
              : requestedPath;

    DropMissingDependencies(rep, tmpPath);
    CapTextures(rep, tmpPath, maxTextureSize);
    NukeReadability(rep, tmpPath, outputPath);

    if (HasExtension(outputPath, ".usdz")) {
        if (!UsdUtilsCreateNewUsdzPackage(SdfAssetPath(tmpPath), outputPath)) {
            rep.Fail("usdz packaging failed for " + outputPath);
            return;
        }
        rep.Info("package",
                 "flattened layer and localized dependencies (incl. UDIM "
                 "textures) packaged into " + outputPath);
        fs::remove(tmpPath, ec);
    } else if (!relinkTextures || !RelinkDependencies(rep, outputPath, tmpPath, requested.stem().string())) {
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
        // Measured in Nuke 16.1 and 17.0: a bench with 78 instances renders
        // fine, ALab's whole set with 1431 stops at "Jpeg read error: Too
        // many open files" - Nuke opens the textures per instance. The
        // de-instanced file of the same set renders, and is no larger
        // (.usdc stores identical data once).
        if (info.counts.instances > 300) {
            rep.Warn("nuke", std::to_string(info.counts.instances) +
                                 " instances kept: Nuke runs out of open files on heavily instanced scenes "
                                 "(it failed at 1431, 78 were fine) - de-instance for Nuke; the file does not grow");
        }
    }
    if (asObj) {
        const bool written = ExportMeshFile(rep, outputPath, requestedPath, frame);
        fs::remove(outputPath, ec);
        if (!written) return;
    }
    rep.outputSizeBytes =
        fs::exists(requestedPath, ec) ? fs::file_size(requestedPath, ec) : 0;
    fs::remove_all(TempDirFor(requestedPath), ec);
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
