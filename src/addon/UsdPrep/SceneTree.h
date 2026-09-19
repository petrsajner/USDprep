// The scene as the artist sees it: one row per object, selection by
// click, keys to walk up and down the hierarchy.
//
// The editor's stage selection is the only selection there is. This
// class reads it every frame and drives it — a click in the 3D view, a
// click here and an arrow key all end up in the same place.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>

namespace usdprep_addon {

class SceneTree {
public:
    // Search field plus the tree, filling the panel down to `reservedBelow`
    // pixels from its bottom (the export section lives there).
    void Draw(const pxr::UsdStageRefPtr& stage, float reservedBelow);

    // Up/Down/F. Runs every frame, whether or not the panel is the front
    // tab — that is what makes the keys global.
    void HandleGlobalKeys(const pxr::UsdStageRefPtr& stage);

    // Selected objects without a selected ancestor, sorted: what an
    // export takes.
    static std::vector<pxr::SdfPath> ExportRoots(const pxr::UsdStageRefPtr& stage);

    // Type size of the tree relative to the rest of the app.
    float fontScale = 1.3f;

private:
    void ObserveSelection(const pxr::UsdStageRefPtr& stage);
    void UpdateFilter(const pxr::UsdStageRefPtr& stage);
    void DrawRow(const pxr::UsdPrim& prim, bool parentSelected, bool parentIsMatch);
    void OnRowClicked(const pxr::SdfPath& path, bool selected);
    void ApplySelection(std::vector<pxr::SdfPath> paths);
    void RevealPath(const pxr::SdfPath& path);
    pxr::SdfPath Anchor() const;
    void SelectParent();
    void SelectChild();

    pxr::UsdStageRefPtr _stage;               // owner of every cache below
    std::set<pxr::SdfPath> _selected;         // this frame's selection
    std::vector<pxr::SdfPath> _lastSeen;      // selection as of last frame
    std::vector<pxr::SdfPath> _lastApplied;   // the last selection this panel set
    std::set<pxr::SdfPath> _pendingOpen;      // rows forced open this frame
    pxr::SdfPath _revealTarget;               // row scrolled to the middle this frame
    std::map<pxr::SdfPath, pxr::SdfPath> _descent;  // parent -> the child we came up from
    char _filter[128] = "";
    std::string _filterApplied;
    std::set<pxr::SdfPath> _directMatches;    // names containing the filter text
    std::set<pxr::SdfPath> _matches;          // those plus every ancestor
};

}  // namespace usdprep_addon
