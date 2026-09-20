// UsdPrep — the "prepare for Nuke" panel.
//
// One tab in the right panel, for someone who does not think in prims:
// find the object, see it in 3D, export it. The editor's stage selection
// is the single source of truth; the tree (SceneTree) reads and drives
// it, the export section (ExportPanel) consumes it, usdprep-core does
// the work.
//
// UX rule: this panel speaks to comp artists, not USD engineers.

#include "addons/Api.h"
#include "Editor.h"
#include "Gui.h"

#include <cstdio>
#include <string>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include "ExportPanel.h"
#include "OutputPath.h"
#include "SceneTree.h"

namespace {

using namespace usdprep_addon;

constexpr const char* kAddonId = "UsdPrep";
constexpr const char* kPanelTitle = "Prep for Nuke";
// Type size of the whole panel relative to the rest of the app.
constexpr float kPanelFontScale = 1.3f;

SceneTree& Tree() {
    static SceneTree tree;
    return tree;
}

ExportPanel& Exporter() {
    static ExportPanel panel;
    static const bool wired = [] {
        panel.onPick = [](const SdfPath& path) { Tree().Select(path); };
        return true;
    }();
    (void)wired;
    return panel;
}

// Objects in the scene, counted once per stage — the header shows it.
int ObjectCount(const UsdStageRefPtr& stage) {
    static UsdStageRefPtr countedStage;
    static int count = 0;
    if (countedStage != stage) {
        countedStage = stage;
        count = 0;
        for (const UsdPrim& prim :
             UsdPrimRange(stage->GetPseudoRoot(), UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            if (!prim.IsPseudoRoot()) ++count;
        }
    }
    return count;
}

// Simple mode, applied once per installation: the panels an artist does
// not need go away (the Windows menu brings any of them back), and this
// panel takes the right side. From then on the layout is the user's.
void ApplySimpleLayoutOnce() {
    static int frame = 0;
    ++frame;
    if (frame == 2) {
        // second frame: our window exists, docking can be applied to it.
        // The version lets a later release re-apply a changed simple mode
        // once, without touching a layout the user has since arranged.
        constexpr const char* kLayoutVersion = "2";
        if (usdtweak::GetAddonString(kAddonId, "layoutVersion", "0") == kLayoutVersion) return;
        if (Editor* editor = usdtweak::GetEditor()) {
            EditorSettings& settings = editor->GetSettingsForAddons();
            settings._showOutliner = false;
            settings._showPropertyEditor = false;
            settings._showTimeline = false;
            settings._showContentBrowser = false;
            settings._showSdfAttributeEditor = false;
            settings._showLayerHierarchyEditor = false;
            settings._showLayerStackEditor = false;
            settings._showPrimSpecEditor = false;
            settings._textEditor = false;
            settings._showUsdConnectionEditor = false;
            settings._showDebugWindow = false;
            settings._showSearch = false;
        }
        if (ImGuiWindowSettings* outliner = ImGui::FindWindowSettingsByID(ImHashStr("Stage outliner"))) {
            if (outliner->DockId != 0) ImGui::DockBuilderDockWindow(kPanelTitle, outliner->DockId);
        }
        usdtweak::SetAddonString(kAddonId, "layoutVersion", kLayoutVersion);
        usdtweak::PersistSettings();
    } else if (frame == 3) {
        ImGui::SetWindowFocus(kPanelTitle);
    }
}

// usdtweak switches the content browser on every time a stage is opened.
// In simple mode that is noise: when the flag flips on in the very frame
// the stage changes, flip it back. A choice the user made in the Windows
// menu does not coincide with a stage change and is left alone.
void KeepContentBrowserHidden(const UsdStageRefPtr& stage) {
    static UsdStageRefPtr lastStage;
    static bool lastShown = false;
    Editor* editor = usdtweak::GetEditor();
    if (!editor) return;
    bool& shown = editor->GetSettingsForAddons()._showContentBrowser;
    if (stage != lastStage && shown && !lastShown) shown = false;
    lastStage = stage;
    lastShown = shown;
}

void DrawPrepPanel() {
    ApplySimpleLayoutOnce();

    const UsdStageRefPtr stage = usdtweak::GetCurrentStage();
    KeepContentBrowserHidden(stage);
    if (!stage) {
        ImGui::TextWrapped("Open a USD scene (File > Open), then pick the objects you want "
                           "to take out - here in the tree or by clicking them in the 3D view.");
        return;
    }

    Tree().HandleGlobalKeys(stage);

    // One type size for the whole panel, larger than the editor around it.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kPanelFontScale);

    // ----- header --------------------------------------------------------
    std::string sceneName = stage->GetRootLayer() ? stage->GetRootLayer()->GetDisplayName() : "";
    if (sceneName.empty()) sceneName = "untitled scene";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(sceneName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d objects", ObjectCount(stage));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() -
                    ImGui::CalcTextSize("Clear selection").x - ImGui::GetStyle().FramePadding.x * 2.0f);
    const std::vector<SdfPath> roots = SceneTree::ExportRoots(stage);
    if (roots.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Clear selection")) {
        // same route as the tree takes: after the frame, through the queue
        ExecuteAfterDraw([](UsdStageRefPtr s) {
            if (Editor* editor = usdtweak::GetEditor()) editor->GetSelection().Clear(s);
        }, stage);
    }
    if (roots.empty()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Up/Down: parent / child of the selected object\n"
                          "F: frame the 3D view on it\n"
                          "Ctrl+click: add or remove without touching the rest");
    }

    // ----- tree, then export ---------------------------------------------
    Tree().Draw(stage, Exporter().LastHeight());
    Exporter().Draw(stage, roots);

    ImGui::PopFont();
}

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, UsdPrep) {
    UsdTweakAddon addon;
    addon.id = kAddonId;
    addon.menuLabel = kPanelTitle;
    addon.kind = UsdTweakAddon::Kind::Window;
    addon.defaultOpen = true;
    addon.draw = &DrawPrepPanel;
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}

}  // namespace
