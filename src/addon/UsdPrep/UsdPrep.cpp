// UsdPrep — the "prepare for Nuke" addon.
//
// Primary workflow (the visual path): open a stage, pick one or more
// objects in the viewport or hierarchy, hit "Export selection", get a
// standalone flattened .usdz/.usdc with de-instancing, defaultPrim and
// localized textures. All heavy lifting lives in usdprep-core; this addon
// is only the panel.

#include "addons/Api.h"
#include "Gui.h"
#include "Selection.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <usdprep/Extract.h>

namespace {

constexpr const char* kAddonId = "UsdPrep";

std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string SuggestOutputPath(const UsdStageRefPtr& stage, const SdfPath& firstPrim) {
    std::string dir = usdtweak::GetAddonString(kAddonId, "lastDir", "");
    if (dir.empty() && stage && stage->GetRootLayer()) {
        const std::string realPath = stage->GetRootLayer()->GetRealPath();
        if (!realPath.empty()) dir = DirectoryOf(realPath);
    }
    if (dir.empty()) dir = ".";
    std::string name = firstPrim.IsEmpty() ? std::string("asset")
                                           : firstPrim.GetName();
    if (name.empty()) name = "asset";
    return dir + "/" + name + ".usdz";
}

void DrawPrepAddon() {
    const UsdStageRefPtr stage = usdtweak::GetCurrentStage();
    if (!stage) {
        ImGui::TextUnformatted("Open a USD stage, select objects, then export.");
        return;
    }

    // Gather the stage selection, collapsed to prim paths (viewport picks
    // can carry property paths).
    std::vector<SdfPath> primPaths;
    for (const SdfPath& p : usdtweak::GetSelection().GetSelectedPaths(stage)) {
        const SdfPath prim = p.IsPrimPath() ? p : p.GetPrimPath();
        if (prim.IsPrimPath() && !prim.IsAbsoluteRootPath() &&
            std::find(primPaths.begin(), primPaths.end(), prim) == primPaths.end()) {
            primPaths.push_back(prim);
        }
    }

    ImGui::Text("Selection: %d object(s)", static_cast<int>(primPaths.size()));
    if (!primPaths.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        const size_t shown = std::min<size_t>(primPaths.size(), 12);
        for (size_t i = 0; i < shown; ++i) {
            ImGui::TextUnformatted(primPaths[i].GetAsString().c_str());
        }
        if (primPaths.size() > shown) {
            ImGui::Text("... and %d more", static_cast<int>(primPaths.size() - shown));
        }
        ImGui::EndTooltip();
    }

    static char outputPath[512] = "";
    if (outputPath[0] == '\0' && !primPaths.empty()) {
        const std::string suggested = SuggestOutputPath(stage, primPaths.front());
        std::snprintf(outputPath, sizeof(outputPath), "%s", suggested.c_str());
    }
    ImGui::InputText("Output", outputPath, sizeof(outputPath));
    ImGui::SameLine();
    static bool usdz = true;
    ImGui::Checkbox(".usdz", &usdz);
    // Keep the extension in sync with the checkbox when it was defaulted.
    if (usdz || outputPath[0] != '\0') {
        const std::string path(outputPath);
        const bool hasExt = path.size() > 5 &&
                            (path.compare(path.size() - 5, 5, ".usdz") == 0 ||
                             path.compare(path.size() - 5, 5, ".usdc") == 0 ||
                             path.compare(path.size() - 5, 5, ".usda") == 0);
        if (!hasExt) {
            std::snprintf(outputPath + path.size(), sizeof(outputPath) - path.size(),
                          usdz ? ".usdz" : ".usdc");
        }
    }

    static bool deinstance = true;
    static bool setDefaultPrim = true;
    ImGui::Checkbox("De-instance", &deinstance);
    ImGui::SameLine();
    ImGui::Checkbox("Set defaultPrim", &setDefaultPrim);

    static std::string lastReport;
    const bool canExport = !primPaths.empty() && outputPath[0] != '\0';
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export selection", ImVec2(-1.0f, 0.0f))) {
        usdprep::ExtractOptions options;
        options.primPaths.reserve(primPaths.size());
        for (const SdfPath& p : primPaths) {
            options.primPaths.push_back(p.GetAsString());
        }
        options.outputPath = outputPath;
        options.deinstance = deinstance;
        options.setDefaultPrim = setDefaultPrim;

        const std::string stagePath = stage->GetRootLayer()->GetRealPath();
        const auto t0 = std::chrono::steady_clock::now();
        const usdprep::Report rep = usdprep::ExtractPrims(stagePath, options);
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        char header[128];
        std::snprintf(header, sizeof(header), "%s in %.2f s — %s\n",
                      rep.ok ? "DONE" : "FAILED", secs, outputPath);
        lastReport = header + rep.ToText();

        if (rep.ok) {
            usdtweak::SetAddonString(kAddonId, "lastDir", DirectoryOf(outputPath));
            usdtweak::PersistSettings();
        }
    }
    if (!canExport) ImGui::EndDisabled();

    if (!primPaths.empty() && ImGui::IsItemHovered() && !canExport) {
        ImGui::SetTooltip("Select objects in the viewport or hierarchy first.");
    }

    if (!lastReport.empty()) {
        ImGui::Separator();
        ImGui::BeginChild("report", ImVec2(0.0f, 0.0f), ImGuiChildFlags_ResizeY);
        ImGui::TextUnformatted(lastReport.c_str());
        ImGui::EndChild();
    }
}

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, UsdPrep) {
    UsdTweakAddon addon;
    addon.id = "UsdPrep";
    addon.menuLabel = "Prep: Export for Nuke";
    addon.kind = UsdTweakAddon::Kind::Window;
    addon.draw = &DrawPrepAddon;
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}

}  // namespace
