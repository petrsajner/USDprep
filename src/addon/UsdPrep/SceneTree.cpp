#include "SceneTree.h"

#include <algorithm>
#include <cctype>

#include "addons/Api.h"
#include "Editor.h"
#include "Gui.h"
#include "Selection.h"

#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

namespace usdprep_addon {

namespace {

// A click on a mesh in 3D means the object it belongs to: the nearest
// ancestor (the prim itself included) that the scene marks as a model
// with the `kind` metadata - the component in a production scene, an
// assembly where no component exists. A scene without kinds gives the
// mesh itself, and the arrow keys take it from there.
SdfPath ObjectFor(const UsdStageRefPtr& stage, const SdfPath& path) {
    const KindRegistry& kinds = KindRegistry::GetInstance();
    for (UsdPrim prim = stage->GetPrimAtPath(path); prim && !prim.IsPseudoRoot();
         prim = prim.GetParent()) {
        TfToken kind;
        if (UsdModelAPI(prim).GetKind(&kind) && kinds.IsA(kind, KindTokens->model)) {
            return prim.GetPath();
        }
    }
    return path;
}

// Selected: unmistakable. Underneath a selected object: still yellow, so
// "and everything under it" is seen rather than assumed.
const ImVec4 kSelectedText(1.0f, 0.85f, 0.2f, 1.0f);
const ImVec4 kChildOfSelectedText(0.78f, 0.68f, 0.32f, 1.0f);

// What the tree shows: defined, active, loaded prims, instance content
// included, abstract "class" prims left out — those are for engineers.
Usd_PrimFlagsPredicate ShownPrims() {
    return UsdTraverseInstanceProxies(UsdPrimDefaultPredicate);
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
    return it != haystack.end();
}

// The editor selection as prim paths: a selected property counts as its
// prim, the pseudo-root is nothing anyone can export.
std::vector<SdfPath> SelectedPrimPaths(const UsdStageRefPtr& stage) {
    std::vector<SdfPath> out;
    for (const SdfPath& p : usdtweak::GetSelection().GetSelectedPaths(stage)) {
        const SdfPath prim = p.IsPrimPath() ? p : p.GetPrimPath();
        if (prim.IsPrimPath() && !prim.IsAbsoluteRootPath()) out.push_back(prim);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------

std::vector<SdfPath> SceneTree::ExportRoots(const UsdStageRefPtr& stage) {
    const std::vector<SdfPath> selected = SelectedPrimPaths(stage);
    std::vector<SdfPath> roots;
    for (const SdfPath& path : selected) {
        bool covered = false;
        for (SdfPath p = path.GetParentPath(); p.GetPathElementCount() >= 1;
             p = p.GetParentPath()) {
            if (std::binary_search(selected.begin(), selected.end(), p)) {
                covered = true;
                break;
            }
        }
        if (!covered) roots.push_back(path);
    }
    return roots;
}

// Editor state may not change mid-draw: every mutation goes through the
// command queue and lands after the frame.
void SceneTree::ApplySelection(std::vector<SdfPath> paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    _lastApplied = paths;
    const auto apply = [](UsdStageRefPtr, const std::vector<SdfPath>& paths) {
        if (paths.empty()) {
            if (Editor* editor = usdtweak::GetEditor()) {
                editor->GetSelection().Clear(usdtweak::GetCurrentStage());
            }
            return;
        }
        usdtweak::SetStagePathSelection(paths.front());
        for (size_t i = 1; i < paths.size(); ++i) {
            usdtweak::AddStagePathSelection(paths[i]);
        }
    };
    ExecuteAfterDraw(apply, _stage, paths);
}

void SceneTree::ObserveSelection(const UsdStageRefPtr& stage) {
    if (_stage != stage) {
        _stage = stage;
        _selected.clear();
        _lastSeen.clear();
        _lastApplied.clear();
        _pendingOpen.clear();
        _revealTarget = SdfPath();
        _descent.clear();
        _filter[0] = '\0';
        _filterApplied.clear();
        _directMatches.clear();
        _matches.clear();
    }

    const std::vector<SdfPath> now = SelectedPrimPaths(stage);
    if (now != _lastSeen) {
        // A change we did not make ourselves came from the 3D view (or
        // another panel): lift what was picked to the object it belongs
        // to, then show where it landed.
        if (now != _lastApplied) {
            std::vector<SdfPath> next = now;
            SdfPath target;
            bool lifted = false;
            for (const SdfPath& p : now) {
                if (std::binary_search(_lastSeen.begin(), _lastSeen.end(), p)) continue;
                const SdfPath object = ObjectFor(stage, p);
                if (object != p) {
                    std::replace(next.begin(), next.end(), p, object);
                    lifted = true;
                }
                if (target.IsEmpty()) target = object;
            }
            if (lifted) ApplySelection(next);
            if (target.IsEmpty()) {
                target = usdtweak::GetSelection().GetAnchorPrimPath(stage);
                if (!target.IsEmpty() && !target.IsPrimPath()) target = target.GetPrimPath();
                if (!std::binary_search(now.begin(), now.end(), target)) target = SdfPath();
            }
            if (!target.IsEmpty()) RevealPath(target);
        }
        _lastSeen = now;
    }
    _selected.clear();
    _selected.insert(now.begin(), now.end());
}

// Unfold exactly the parents needed, scroll the object to the middle,
// leave the object itself folded. A search would hide it, so it goes.
void SceneTree::RevealPath(const SdfPath& path) {
    _pendingOpen.clear();
    for (SdfPath p = path.GetParentPath(); p.GetPathElementCount() >= 1; p = p.GetParentPath()) {
        _pendingOpen.insert(p);
    }
    _revealTarget = path;
    _filter[0] = '\0';
}

void SceneTree::OnRowClicked(const SdfPath& path, bool selected) {
    std::vector<SdfPath> next;
    if (selected) {
        // Deselecting an object takes everything under it along, whether
        // it got there by inheritance or by its own ctrl+click. The rest
        // of the selection stays either way.
        for (const SdfPath& p : _selected) {
            if (!p.HasPrefix(path)) next.push_back(p);
        }
    } else if (ImGui::GetIO().KeyCtrl) {
        next.assign(_selected.begin(), _selected.end());
        next.push_back(path);
    } else {
        // the plain click only ever replaces on the way in
        next.push_back(path);
    }
    ApplySelection(next);
}

// ---------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------

SdfPath SceneTree::Anchor() const {
    if (!_stage) return SdfPath();
    SdfPath anchor = usdtweak::GetSelection().GetAnchorPrimPath(_stage);
    if (!anchor.IsEmpty() && !anchor.IsPrimPath()) anchor = anchor.GetPrimPath();
    if (!anchor.IsEmpty() && _selected.count(anchor)) return anchor;
    const std::vector<SdfPath> roots = ExportRoots(_stage);
    return roots.empty() ? SdfPath() : roots.front();
}

void SceneTree::SelectParent() {
    const SdfPath anchor = Anchor();
    if (anchor.IsEmpty()) return;
    const SdfPath parent = anchor.GetParentPath();
    if (parent.IsEmpty() || parent.IsAbsoluteRootPath()) return;
    _descent[parent] = anchor;
    ApplySelection({parent});
    RevealPath(parent);
}

void SceneTree::SelectChild() {
    if (!_stage) return;
    const SdfPath anchor = Anchor();
    const UsdPrim from = anchor.IsEmpty() ? _stage->GetPseudoRoot() : _stage->GetPrimAtPath(anchor);
    if (!from) return;

    SdfPath child;
    const auto remembered = _descent.find(from.GetPath());
    if (remembered != _descent.end() && _stage->GetPrimAtPath(remembered->second)) {
        child = remembered->second;
    } else {
        const auto children = from.GetFilteredChildren(ShownPrims());
        if (children.begin() != children.end()) child = (*children.begin()).GetPath();
    }
    if (child.IsEmpty()) return;
    ApplySelection({child});
    RevealPath(child);
}

void SceneTree::HandleGlobalKeys(const UsdStageRefPtr& stage) {
    if (!stage) return;
    if (_stage != stage) ObserveSelection(stage);
    // A text field owns the keyboard while the artist types in it.
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
        SelectParent();
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
        SelectChild();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        usdtweak::FrameCameraOnSelection();
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void SceneTree::UpdateFilter(const UsdStageRefPtr& stage) {
    const std::string text(_filter);
    if (text == _filterApplied) return;
    _filterApplied = text;
    _directMatches.clear();
    _matches.clear();
    if (text.empty()) return;
    for (const UsdPrim& prim : UsdPrimRange(stage->GetPseudoRoot(), ShownPrims())) {
        if (prim.IsPseudoRoot()) continue;
        if (!ContainsCaseInsensitive(prim.GetName().GetString(), text)) continue;
        const SdfPath path = prim.GetPath();
        _directMatches.insert(path);
        // every ancestor is on the way to a match; once one is already
        // known, so are all of its own ancestors
        for (SdfPath p = path; p.GetPathElementCount() >= 1; p = p.GetParentPath()) {
            if (!_matches.insert(p).second) break;
        }
    }
}

void SceneTree::DrawRow(const UsdPrim& prim, bool parentSelected, bool parentIsMatch) {
    const SdfPath path = prim.GetPath();
    const bool filtering = !_filterApplied.empty();
    const bool isMatch = filtering && _directMatches.count(path) > 0;
    const bool onMatchPath = filtering && _matches.count(path) > 0;
    // While searching: matches, their parents, and the children of a
    // match once it is unfolded. Nothing else.
    if (filtering && !onMatchPath && !parentIsMatch) return;

    const bool selected = _selected.count(path) > 0;
    const bool inherited = !selected && parentSelected;

    std::vector<UsdPrim> children;
    for (const UsdPrim& child : prim.GetFilteredChildren(ShownPrims())) children.push_back(child);
    const bool hasChildren = !children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanFullWidth;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    if (_pendingOpen.count(path) || (onMatchPath && !isMatch)) ImGui::SetNextItemOpen(true);
    if (path == _revealTarget) ImGui::SetNextItemOpen(false);

    const ImVec4 color = selected    ? kSelectedText
                         : inherited ? kChildOfSelectedText
                                     : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const bool open =
        ImGui::TreeNodeEx(path.GetText(), flags, "%s", prim.GetName().GetText()) && hasChildren;
    ImGui::PopStyleColor();

    if (path == _revealTarget) ImGui::SetScrollHereY(0.5f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        OnRowClicked(path, selected);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
        ImGui::SetTooltip("%s\n%s", path.GetText(), prim.GetTypeName().GetText());
    }
    if (hasChildren && !open) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d", static_cast<int>(children.size()));
    }
    if (open) {
        for (const UsdPrim& child : children) {
            DrawRow(child, selected || parentSelected, isMatch || parentIsMatch);
        }
        ImGui::TreePop();
    }
}

void SceneTree::Draw(const UsdStageRefPtr& stage, float reservedBelow) {
    ObserveSelection(stage);

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search objects...", _filter, sizeof(_filter));
    UpdateFilter(stage);

    const float fontSize = ImGui::GetStyle().FontSizeBase * fontScale;
    ImGui::PushFont(nullptr, fontSize);
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, fontSize * 1.1f);
    ImGui::BeginChild("scene-tree", ImVec2(0.0f, -reservedBelow), ImGuiChildFlags_Borders);
    for (const UsdPrim& child : stage->GetPseudoRoot().GetFilteredChildren(ShownPrims())) {
        DrawRow(child, false, false);
    }
    if (!_filterApplied.empty() && _directMatches.empty()) {
        ImGui::TextDisabled("  No object called '%s'", _filterApplied.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopFont();

    // one-shot: the reveal was drawn this frame
    _pendingOpen.clear();
    _revealTarget = SdfPath();
}

}  // namespace usdprep_addon
