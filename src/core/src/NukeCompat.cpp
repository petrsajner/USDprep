// What is left in the export after the recipe had its say, made readable
// for Nuke. The rule: nothing is thrown away here - whatever Nuke cannot
// read is converted or replaced by the closest thing it can, and every
// substitution is named in the report. What Nuke does and does not read
// was measured, not assumed: NUKE_COMPAT.md.

#include "Shared.h"

#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdSkel/bakeSkinning.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/root.h>

namespace usdprep {
namespace detail {

// NukeGeometry.cpp: per-face materials, implicit shapes, curves.
void MakeGeometryNukeReadable(Report& rep, const UsdStageRefPtr& flat);

namespace {

// At most `limit` names, then "and N more".
std::string NameList(const std::vector<std::string>& names, size_t limit = 5) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < limit; ++i) out += (i ? ", " : "") + names[i];
    if (names.size() > limit) out += " and " + std::to_string(names.size() - limit) + " more";
    return out;
}

// Nuke draws every purpose, so an asset that carries its own proxy shows
// twice. Kept guide/proxy geometry is hidden instead - it is still in the
// file, and one visibility switch away.
void HideGuidesAndProxies(Report& rep, const UsdStageRefPtr& flat) {
    std::vector<std::string> hidden;
    for (const UsdPrim& prim : flat->Traverse()) {
        const UsdGeomImageable imageable(prim);
        if (!imageable) continue;
        TfToken purpose;
        const UsdAttribute attr = imageable.GetPurposeAttr();
        if (!attr || !attr.HasAuthoredValue() || !attr.Get(&purpose)) continue;
        if (purpose != UsdGeomTokens->proxy && purpose != UsdGeomTokens->guide) continue;
        UsdAttribute visibility = imageable.CreateVisibilityAttr();
        visibility.Clear();  // time samples would win over the value below
        visibility.Set(UsdGeomTokens->invisible);
        hidden.push_back(prim.GetPath().GetString());
    }
    if (!hidden.empty()) {
        rep.Info("nuke", std::to_string(hidden.size()) +
                             " guide/proxy object(s) kept but hidden - Nuke draws every purpose and the "
                             "asset would show twice: " + NameList(hidden));
    }
}

bool HasConnection(const UsdAttribute& attr) {
    SdfPathVector sources;
    return attr && attr.GetConnections(&sources) && !sources.empty();
}

// Two things Nuke does with materials, both measured: a MaterialX output
// next to a UsdPreviewSurface one wins and renders black; and a mesh
// bound to a material with no universal surface at all is not drawn.
void MakeMaterialsRenderable(Report& rep, const UsdStageRefPtr& flat) {
    static const TfToken kSurface("outputs:surface");
    size_t mtlxRemoved = 0;
    std::set<SdfPath> unrenderable;
    for (UsdPrim prim : flat->Traverse()) {
        if (!prim.IsA<UsdShadeMaterial>()) continue;
        const bool universal = HasConnection(prim.GetAttribute(kSurface));
        if (universal) {
            std::vector<TfToken> mtlx;
            for (const UsdAttribute& attr : prim.GetAttributes()) {
                if (attr.GetName().GetString().rfind("outputs:mtlx:", 0) == 0) mtlx.push_back(attr.GetName());
            }
            for (const TfToken& name : mtlx) {
                if (prim.RemoveProperty(name)) ++mtlxRemoved;
            }
        } else {
            unrenderable.insert(prim.GetPath());
        }
    }
    if (mtlxRemoved > 0) {
        rep.Info("nuke", std::to_string(mtlxRemoved) +
                             " MaterialX output(s) removed from materials that also have a standard "
                             "surface - Nuke picks the MaterialX one and renders it black");
    }
    if (unrenderable.empty()) return;

    std::set<std::string> unboundMaterials;
    size_t unboundPrims = 0;
    for (UsdPrim prim : flat->Traverse()) {
        std::vector<UsdRelationship> bindings;
        for (const UsdRelationship& rel : prim.GetRelationships()) {
            if (rel.GetName().GetString().rfind("material:binding", 0) == 0) bindings.push_back(rel);
        }
        bool touched = false;
        for (const UsdRelationship& rel : bindings) {
            SdfPathVector targets;
            rel.GetTargets(&targets);
            SdfPathVector kept;
            for (const SdfPath& target : targets) {
                if (unrenderable.count(target)) {
                    unboundMaterials.insert(target.GetString());
                } else {
                    kept.push_back(target);
                }
            }
            if (kept.size() == targets.size()) continue;
            touched = true;
            if (kept.empty()) {
                prim.RemoveProperty(rel.GetName());
            } else {
                rel.SetTargets(kept);
            }
        }
        if (touched) ++unboundPrims;
    }
    if (unboundPrims > 0) {
        rep.Warn("nuke", std::to_string(unboundPrims) +
                             " object(s) unbound from material(s) with no standard surface (Nuke does not "
                             "draw a mesh bound to one; it now shows its display colour): " +
                             NameList(std::vector<std::string>(unboundMaterials.begin(), unboundMaterials.end())));
    }
}

// Lights that made it into the export (they are off in the nuke preset).
// Nuke lights the scene with sphere, disk and dome lights; a distant light
// gives it nothing but still switches the unlit default off - a black
// render; rect and cylinder lights are ignored. The ones Nuke cannot use
// become plain transforms of the same name, settings still on them, so
// the light can be rebuilt in Nuke where it stood.
void ReplaceUnreadableLights(Report& rep, const UsdStageRefPtr& flat) {
    std::vector<UsdPrim> lights;
    for (const UsdPrim& prim : flat->Traverse()) {
        if (prim.HasAPI<UsdLuxLightAPI>() && !prim.IsA<UsdGeomGprim>()) lights.push_back(prim);
    }
    size_t kept = 0;
    for (UsdPrim prim : lights) {
        const std::string type = prim.GetTypeName().GetString();
        if (type == "SphereLight" || type == "DiskLight" || type == "DomeLight") {
            ++kept;
            continue;
        }
        prim.SetTypeName(TfToken("Xform"));
        rep.Warn("nuke", "light " + prim.GetPath().GetString() + " (" + type +
                             ") replaced by an axis of the same name - Nuke does not read this light "
                             "type; its position and settings are still on it");
    }
    if (kept > 0) {
        rep.Info("nuke", std::to_string(kept) +
                             " light(s) kept. With a light in the file Nuke stops showing surfaces "
                             "unlit - expect a darker picture than without them");
    }
}

// Nuke does not evaluate UsdSkel: a skinned character stands in its bind
// pose. USD's own baker turns the skinning into what Nuke does read - a
// point cache on every skinned mesh. Runs before the animation trim, so
// the trim applies to the baked samples as to any other.
void BakeSkinning(Report& rep, const UsdStageRefPtr& flat) {
    std::vector<std::string> roots;
    for (const UsdPrim& prim : flat->Traverse()) {
        if (prim.IsA<UsdSkelRoot>()) roots.push_back(prim.GetPath().GetString());
    }
    if (roots.empty()) return;
    size_t skinned = 0;
    for (const UsdPrim& prim : flat->Traverse()) {
        if (prim.IsA<UsdGeomGprim>() && prim.HasAPI<UsdSkelBindingAPI>()) ++skinned;
    }
    if (UsdSkelBakeSkinning(flat->Traverse())) {
        rep.Info("nuke", "skeletal animation baked into point caches on " + std::to_string(skinned) +
                             " mesh(es) - Nuke does not evaluate skinning and would show the bind pose: " +
                             NameList(roots));
    } else {
        rep.Warn("nuke", "skeletal animation could not be baked; Nuke will show the bind pose of: " +
                             NameList(roots));
    }
}

// Nuke takes every file as Y-up. A Z-up scene (Houdini, Blender, 3ds Max
// pipelines) arrives lying on its back, so its top-level prims get one
// rotation in front of their own transform and the stage says Y.
void ConvertToYUp(Report& rep, const UsdStageRefPtr& flat) {
    if (UsdGeomGetStageUpAxis(flat) != UsdGeomTokens->z) return;
    std::vector<std::string> rotated;
    for (UsdPrim prim : flat->GetPseudoRoot().GetChildren()) {
        // an exposed prototype is not part of the scene: its instances get the rotation from their own roots
        if (ExposedPrototypes::IsPrototypeName(prim.GetName().GetString())) continue;
        if (!prim.IsA<UsdGeomXformable>()) {
            // a Scope or an untyped group cannot carry a transform; an Xform is the same thing that can
            if (!prim.GetTypeName().IsEmpty() && prim.GetTypeName() != "Scope") continue;
            prim.SetTypeName(TfToken("Xform"));
        }
        UsdGeomXformable xformable(prim);
        bool resets = false;
        std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
        const UsdGeomXformOp yUp =
            xformable.AddRotateXOp(UsdGeomXformOp::PrecisionFloat, TfToken("usdprepYUp"));
        yUp.Set(-90.0f);
        ops.insert(ops.begin(), yUp);
        xformable.SetXformOpOrder(ops, resets);
        rotated.push_back(prim.GetPath().GetString());
    }
    UsdGeomSetStageUpAxis(flat, UsdGeomTokens->y);
    rep.Info("nuke", "the scene was Z-up; Nuke reads every file as Y-up, so " + NameList(rotated) +
                         " got a -90 degree X rotation (xformOp:rotateX:usdprepYUp) and the file now says Y-up");
}

}  // namespace

void MakeNukeReadable(Report& rep, const UsdStageRefPtr& flat) {
    BakeSkinning(rep, flat);  // first: the split below copies the baked points
    MakeGeometryNukeReadable(rep, flat);
    ConvertToYUp(rep, flat);
    HideGuidesAndProxies(rep, flat);
    MakeMaterialsRenderable(rep, flat);
    ReplaceUnreadableLights(rep, flat);
}

}  // namespace detail
}  // namespace usdprep
