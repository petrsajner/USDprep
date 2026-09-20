// "Whole scene" for someone who got lost: not the bounding box of every
// prim - a production scene has a backdrop, a garden, a sky dome, and the
// box around all of that puts the camera out in the garden - but the box
// around where the objects are.
#pragma once

#include <algorithm>
#include <vector>

#include "addons/Api.h"

#include <pxr/base/gf/range3d.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>

namespace usdprep_addon {

// The objects are the scene's components (the prims marked with `kind`),
// or its meshes where nothing is marked. Their centres say where things
// are: the middle 80 % on every axis, grown by a typical object's size.
// A handful of far-flung or huge pieces then no longer decide the view.
inline pxr::GfRange3d WhereThingsAre(const pxr::UsdStageRefPtr& stage) {
    using namespace pxr;
    UsdGeomBBoxCache cache(UsdTimeCode(stage->GetStartTimeCode()),
                           {UsdGeomTokens->default_, UsdGeomTokens->render}, /*useExtentsHint=*/true);
    const KindRegistry& kinds = KindRegistry::GetInstance();
    std::vector<GfRange3d> boxes;
    const auto collect = [&](bool components) {
        UsdPrimRange range(stage->GetPseudoRoot(), UsdTraverseInstanceProxies(UsdPrimDefaultPredicate));
        for (auto it = range.begin(); it != range.end(); ++it) {
            bool take = false;
            if (components) {
                TfToken kind;
                take = UsdModelAPI(*it).GetKind(&kind) && kinds.IsA(kind, KindTokens->component);
            } else {
                take = it->IsA<UsdGeomMesh>();
            }
            if (!take) continue;
            const GfRange3d box = cache.ComputeWorldBound(*it).ComputeAlignedRange();
            if (!box.IsEmpty()) boxes.push_back(box);
            it.PruneChildren();
        }
    };
    collect(true);
    if (boxes.size() < 8) {
        boxes.clear();
        collect(false);
    }
    if (boxes.empty()) return GfRange3d();

    std::vector<double> sizes;
    for (const GfRange3d& box : boxes) sizes.push_back(box.GetSize().GetLength());
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    const double typical = sizes[sizes.size() / 2];

    GfVec3d low(0.0), high(0.0);
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<double> centres;
        for (const GfRange3d& box : boxes) centres.push_back(box.GetMidpoint()[axis]);
        std::sort(centres.begin(), centres.end());
        const size_t cut = boxes.size() >= 10 ? centres.size() / 10 : 0;
        low[axis] = centres[cut] - typical;
        high[axis] = centres[centres.size() - 1 - cut] + typical;
    }
    return GfRange3d(low, high);
}

// The way back from anywhere. Worked out once per scene (it walks every
// object), then it is a camera move.
inline void FrameOverview(const pxr::UsdStageRefPtr& stage) {
    static pxr::UsdStageRefPtr knownStage;
    static pxr::GfRange3d known;
    if (!stage) return;
    if (knownStage != stage) {
        knownStage = stage;
        known = WhereThingsAre(stage);
    }
    if (known.IsEmpty()) {
        usdtweak::FrameCameraOnScene();
    } else {
        // the objects usually sit in a room: cut into the box far enough to look inside
        usdtweak::FrameCameraOnBox(known, 0.45);
    }
}

}  // namespace usdprep_addon
