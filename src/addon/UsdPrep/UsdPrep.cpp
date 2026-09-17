// UsdPrep — the "prepare for Nuke" addon.
//
// Primary workflow (the visual path): open a stage, pick one or more
// objects in the viewport or hierarchy, hit "Export selection", get a
// standalone flattened .usdz/.usdc with de-instancing, defaultPrim and
// localized textures. All heavy lifting lives in usdprep-core; this addon
// is only the panel.
//
// UX rule: this panel speaks to comp artists, not USD engineers. Technical
// options live under "Advanced" and are explained in plain language.

#include "addons/Api.h"
#include "Gui.h"
#include "Selection.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <usdprep/Extract.h>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace {

constexpr const char* kAddonId = "UsdPrep";

// ---------------------------------------------------------------------------
// small path helpers
// ---------------------------------------------------------------------------

std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string BasenameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool HasUsdExtension(const std::string& path) {
    return path.size() > 5 &&
           (path.compare(path.size() - 5, 5, ".usdz") == 0 ||
            path.compare(path.size() - 5, 5, ".usdc") == 0 ||
            path.compare(path.size() - 5, 5, ".usda") == 0);
}

std::string WithExtension(const std::string& path, const char* ext) {
    std::string p = path;
    if (HasUsdExtension(p)) p.resize(p.size() - 5);
    return p + ext;
}

#ifdef _WIN32

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], size);
    return out;
}

// Native "Save as..." dialog. Returns false when the user cancelled.
bool NativeSaveDialog(const std::string& suggestedName, bool usdz, std::string& outPath) {
    static const bool comInitialized = [] {
        const HRESULT hr =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    if (!comInitialized) return false;

    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"USD package (*.usdz)", L"*.usdz"},
        {L"USD layer (*.usdc)", L"*.usdc"},
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(usdz ? 1 : 2);
    dialog->SetDefaultExtension(usdz ? L"usdz" : L"usdc");
    const std::wstring suggested = Utf8ToWide(suggestedName);
    if (!suggested.empty()) dialog->SetFileName(suggested.c_str());

    const bool shown = SUCCEEDED(dialog->Show(nullptr /* no owner: keep it simple */));
    std::string result;
    if (shown) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// the panel
// ---------------------------------------------------------------------------

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

    // ----- what gets exported: the objects, one per line -------------
    ImGui::Text("Exporting %d object(s):", static_cast<int>(primPaths.size()));
    if (primPaths.empty()) {
        ImGui::TextDisabled("    (nothing selected — click objects in the 3D view)");
    } else {
        ImGui::BeginChild("selection",
                          ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 6.0f),
                          ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);
        for (const SdfPath& p : primPaths) {
            ImGui::Bullet();
            ImGui::TextUnformatted(p.GetName().c_str());
            ImGui::SameLine(180.0f);
            ImGui::TextDisabled("%s", p.GetAsString().c_str());
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // ----- where it goes: path + native save dialog -------------------
    static char outputPath[512] = "";
    if (outputPath[0] == '\0' && !primPaths.empty()) {
        const std::string suggested = SuggestOutputPath(stage, primPaths.front());
        std::snprintf(outputPath, sizeof(outputPath), "%s", suggested.c_str());
    }

    static int format = 0;  // 0 = .usdz package (recommended), 1 = .usdc layer
    const char* formatNames[] = {
        "Package  (.usdz)  — one file, textures included (recommended)",
        "Layer    (.usdc)  — geometry only, textures stay outside",
    };

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
    ImGui::InputText("##output", outputPath, sizeof(outputPath));
    ImGui::PopItemWidth();
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Browse...")) {
        std::string chosen;
        const std::string current(outputPath);
        const std::string suggestion = HasUsdExtension(current)
                                           ? BasenameOf(current)
                                           : BasenameOf(current) + (format == 0 ? ".usdz" : ".usdc");
        if (NativeSaveDialog(suggestion, format == 0, chosen)) {
            std::snprintf(outputPath, sizeof(outputPath), "%s", chosen.c_str());
            if (chosen.size() > 5 && chosen.compare(chosen.size() - 5, 5, ".usdz") != 0) {
                format = 1;  // user picked a .usdc (or other) name in the dialog
            } else {
                format = 0;
            }
        }
    }
#endif

    ImGui::Combo("##format", &format, formatNames, 2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Package (.usdz): everything in one file — geometry, materials and\n"
            "textures. Best for sharing and for Nuke.\n"
            "Layer (.usdc): geometry and materials only; texture files stay\n"
            "where they are (paths are not adjusted yet).");
    }
    // Keep the extension of the output path in sync with the format choice.
    if (outputPath[0] != '\0') {
        const std::string synced =
            WithExtension(outputPath, format == 0 ? ".usdz" : ".usdc");
        if (synced != outputPath) {
            std::snprintf(outputPath, sizeof(outputPath), "%s", synced.c_str());
        }
    }

    // ----- technical options stay out of the artist's way --------------
    static bool deinstance = true;
    static bool setDefaultPrim = true;
    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::Checkbox("De-instance", &deinstance);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Converts USD instancing into plain, standalone objects so the\n"
                "file reads the same everywhere. Recommended.");
        }
        ImGui::Checkbox("Set main object", &setDefaultPrim);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Marks the main object of the exported file — like the object\n"
                "name inside an .obj file. Applications use it to know what to\n"
                "load when the file is imported. Recommended.");
        }
    }

    ImGui::Separator();

    // ----- run ---------------------------------------------------------
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

        char header[160];
        std::snprintf(header, sizeof(header), "%s in %.2f s — %s\n",
                      rep.ok ? "DONE" : "FAILED", secs, outputPath);
        lastReport = header + rep.ToText();

        if (rep.ok) {
            usdtweak::SetAddonString(kAddonId, "lastDir", DirectoryOf(outputPath));
            usdtweak::PersistSettings();
        }
    }
    if (!canExport) ImGui::EndDisabled();

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
