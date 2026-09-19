#include "ExportPanel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include "addons/Api.h"
#include "Gui.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Extract.h>

#include "OutputPath.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace usdprep_addon {

namespace {

constexpr const char* kAddonId = "UsdPrep";

const char* PresetLabel(const std::string& name) {
    if (name == "nuke") return "Nuke-ready";
    if (name == "raw") return "Raw copy";
    return name.c_str();
}

std::string HumanSize(uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", bytes / 1024.0);
    }
    return buffer;
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

bool EnsureCom() {
    static const bool initialized = [] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    return initialized;
}

// Runs a shown file dialog to its result path. Empty = cancelled.
std::string DialogResult(IFileDialog* dialog) {
    std::string result;
    if (SUCCEEDED(dialog->Show(nullptr))) {
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
    return result;
}

// Native "Save as...". False = cancelled.
bool NativeSaveDialog(const std::string& suggestedName, bool usdz, std::string& outPath) {
    if (!EnsureCom()) return false;
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
    const std::string result = DialogResult(dialog);
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

// Native "Open" for a recipe file. False = cancelled.
bool NativeOpenRecipeDialog(std::string& outPath) {
    if (!EnsureCom()) return false;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    const COMDLG_FILTERSPEC filters[] = {{L"Recipe (*.json)", L"*.json"}};
    dialog->SetFileTypes(1, filters);
    const std::string result = DialogResult(dialog);
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

#endif  // _WIN32

}  // namespace

// ---------------------------------------------------------------------------
// recipe choice
// ---------------------------------------------------------------------------

void ExportPanel::ChoosePreset(int choice, const std::string& recipePath) {
    const std::vector<std::string> presets = usdprep::PresetNames();
    _presetChoice = choice;
    _recipePath = recipePath;
    _recipeProblem.clear();
    if (choice >= 0 && choice < static_cast<int>(presets.size())) {
        usdprep::GetPreset(presets[choice], &_recipe);
    } else {
        std::string error;
        std::vector<std::string> warnings;
        usdprep::Recipe loaded;
        if (usdprep::LoadRecipe(recipePath, &loaded, &error, &warnings)) {
            _recipe = loaded;
            if (!warnings.empty()) _recipeProblem = warnings.front();
        } else {
            _recipeProblem = error;
            usdprep::GetPreset("nuke", &_recipe);
        }
    }
    // The switches start where the recipe puts them; the artist can
    // still flip any of them for one run.
    _deinstance = _recipe.deinstance;
    _setDefaultPrim = _recipe.setDefaultPrim;
    _relinkTextures = _recipe.relinkTextures;

    usdtweak::SetAddonString(kAddonId, "preset",
                             choice >= 0 && choice < static_cast<int>(presets.size())
                                 ? presets[choice]
                                 : std::string("file"));
    usdtweak::SetAddonString(kAddonId, "recipePath", recipePath);
}

void ExportPanel::DrawPreset() {
    const std::vector<std::string> presets = usdprep::PresetNames();
    if (_presetChoice < 0) {
        // first draw: restore the last choice
        const std::string saved = usdtweak::GetAddonString(kAddonId, "preset", "nuke");
        const std::string savedPath = usdtweak::GetAddonString(kAddonId, "recipePath", "");
        int choice = 0;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (presets[i] == saved) choice = static_cast<int>(i);
        }
        if (saved == "file" && !savedPath.empty()) choice = static_cast<int>(presets.size());
        ChoosePreset(choice, savedPath);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    const std::string current =
        _presetChoice < static_cast<int>(presets.size())
            ? std::string(PresetLabel(presets[_presetChoice]))
            : "Recipe: " + BasenameOf(_recipePath);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##preset", current.c_str())) {
        for (size_t i = 0; i < presets.size(); ++i) {
            const bool isCurrent = _presetChoice == static_cast<int>(i);
            if (ImGui::Selectable(PresetLabel(presets[i]), isCurrent)) {
                ChoosePreset(static_cast<int>(i), "");
            }
            if (ImGui::IsItemHovered()) {
                usdprep::Recipe r;
                usdprep::GetPreset(presets[i], &r);
                ImGui::SetTooltip("%s", r.description.c_str());
            }
        }
#ifdef _WIN32
        if (ImGui::Selectable("Recipe file...", _presetChoice == static_cast<int>(presets.size()))) {
            std::string chosen;
            if (NativeOpenRecipeDialog(chosen)) ChoosePreset(static_cast<int>(presets.size()), chosen);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A studio recipe as JSON. Start from what\n"
                              "'usdcut presets nuke' prints and change what you need.");
        }
#endif
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##preset")) {
        ImGui::SetTooltip("%s", _recipe.description.empty() ? "Your own recipe." : _recipe.description.c_str());
    }
    if (!_recipeProblem.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", _recipeProblem.c_str());
    }
}

// ---------------------------------------------------------------------------
// export list
// ---------------------------------------------------------------------------

void ExportPanel::DrawList(const std::vector<SdfPath>& selectionRoots) {
    const bool canAdd = !selectionRoots.empty();
    if (!canAdd) ImGui::BeginDisabled();
    if (ImGui::Button("Add to export")) {
        for (const SdfPath& p : selectionRoots) {
            if (std::find(_list.begin(), _list.end(), p) == _list.end()) _list.push_back(p);
        }
    }
    if (!canAdd) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keeps the selected objects on the export list, so you can\n"
                          "go on and pick more. Export then takes the whole list.");
    }
    ImGui::SameLine();
    if (_list.empty()) {
        ImGui::TextDisabled("Export list is empty: Export takes the selection.");
        return;
    }
    ImGui::Text("Export list: %d object(s)", static_cast<int>(_list.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear list")) _list.clear();

    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float listHeight = rowHeight * std::min<float>(static_cast<float>(_list.size()), 5.0f) +
                             ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginChild("export-list", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < _list.size(); /*advanced in loop*/) {
        ImGui::PushID(static_cast<int>(i));
        const bool remove = ImGui::SmallButton("x");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the export list");
        ImGui::SameLine();
        ImGui::TextUnformatted(_list[i].GetName().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _list[i].GetText());
        ImGui::PopID();
        if (remove) {
            _list.erase(_list.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// destination
// ---------------------------------------------------------------------------

void ExportPanel::DrawDestination(const UsdStageRefPtr& stage, const std::vector<SdfPath>& targets) {
    // The name follows the first object until the artist edits the path
    // by hand; the folder is the last one used, or the scene's own.
    const std::string firstTarget = targets.empty() ? std::string() : targets.front().GetName();
    if (_suggestedFor != stage) {
        _suggestedFor = stage;
        _pathEdited = false;
        _outputPath[0] = '\0';
    }
    if (!_pathEdited && !targets.empty() && firstTarget != _suggestedName) {
        _suggestedName = firstTarget;
        _outputPath[0] = '\0';
    }
    if (_outputPath[0] == '\0' && !targets.empty()) {
        std::string dir = usdtweak::GetAddonString(kAddonId, "lastDir", "");
        if (dir.empty() && stage && stage->GetRootLayer()) {
            const std::string realPath = stage->GetRootLayer()->GetRealPath();
            if (!realPath.empty()) dir = DirectoryOf(realPath);
        }
        if (dir.empty()) dir = ".";
        std::string name = targets.front().GetName();
        if (name.empty()) name = "asset";
        const std::string suggested = dir + "/" + name + (_format == 0 ? ".usdz" : ".usdc");
        std::snprintf(_outputPath, sizeof(_outputPath), "%s", suggested.c_str());
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Save as");
    ImGui::SameLine();
    const float browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 4.0f;
    ImGui::SetNextItemWidth(-browseWidth);
    if (ImGui::InputText("##output", _outputPath, sizeof(_outputPath))) _pathEdited = true;
    const bool editingPath = ImGui::IsItemActive();
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Browse...")) {
        std::string stem = OutputNameOf(_outputPath);
        if (stem.empty() && !targets.empty()) stem = targets.front().GetName();
        if (stem.empty()) stem = "asset";
        std::string chosen;
        if (NativeSaveDialog(stem + (_format == 0 ? ".usdz" : ".usdc"), _format == 0, chosen)) {
            std::snprintf(_outputPath, sizeof(_outputPath), "%s", chosen.c_str());
            _pathEdited = true;
            _format = (chosen.size() >= 5 && chosen.compare(chosen.size() - 5, 5, ".usdz") == 0) ? 0 : 1;
        }
    }
#else
    ImGui::BeginDisabled();
    ImGui::Button("Browse...");
    ImGui::EndDisabled();
#endif

    const char* formats[] = {
        "Package (.usdz) - one file, textures inside",
        "Layer (.usdc) - file + a folder with its textures",
    };
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##format", &_format, formats, 2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Package: everything in one file. Best for sharing and for Nuke.\n"
                          "Layer: the scene file plus a \"<name>_textures\" folder next to\n"
                          "it. Keep the two together when you move the file.");
    }
    // The extension follows the format - never while the artist is typing.
    if (!editingPath && _outputPath[0] != '\0') {
        const std::string synced = WithExtension(_outputPath, _format == 0 ? ".usdz" : ".usdc");
        if (synced != _outputPath) std::snprintf(_outputPath, sizeof(_outputPath), "%s", synced.c_str());
    }
}

void ExportPanel::DrawAdvanced() {
    if (!ImGui::CollapsingHeader("Advanced")) return;
    ImGui::Checkbox("De-instance", &_deinstance);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Turns USD instancing into plain, standalone objects so the\n"
                          "file reads the same everywhere. Recommended.");
    }
    ImGui::Checkbox("Set main object", &_setDefaultPrim);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Marks the main object of the exported file. Applications use\n"
                          "it to know what to load. Recommended.");
    }
    const bool isLayer = _format == 1;
    if (!isLayer) ImGui::BeginDisabled();
    ImGui::Checkbox("Copy textures next to the file", &_relinkTextures);
    if (!isLayer) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(isLayer ? "Copies every texture the objects use into a folder next to\n"
                                    "the exported file and points the file at the copies.\n"
                                    "Recommended."
                                  : "Only applies to the .usdc format - a .usdz package\n"
                                    "always carries its textures inside.");
    }
    if (!_recipe.dropTypes.empty() || !_recipe.dropPurposes.empty()) {
        std::string removed;
        for (const std::string& t : _recipe.dropTypes) removed += (removed.empty() ? "" : ", ") + t;
        for (const std::string& p : _recipe.dropPurposes) {
            removed += (removed.empty() ? "" : ", ") + p + " geometry";
        }
        ImGui::TextDisabled("This preset also removes: %s", removed.c_str());
    }
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

void ExportPanel::Run(const UsdStageRefPtr& stage, const std::vector<SdfPath>& targets) {
    usdprep::ExtractOptions options;
    usdprep::ApplyRecipe(_recipe, &options);
    options.deinstance = _deinstance;
    options.setDefaultPrim = _setDefaultPrim;
    options.relinkTextures = _relinkTextures;
    options.outputPath = _outputPath;
    for (const SdfPath& p : targets) options.primPaths.push_back(p.GetAsString());

    const std::string stagePath = stage->GetRootLayer()->GetRealPath();
    const auto t0 = std::chrono::steady_clock::now();
    const usdprep::Report rep = usdprep::ExtractPrims(stagePath, options);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    _resultOk = rep.ok;
    _report = rep.ToText();
    char line[512];
    if (rep.ok) {
        std::snprintf(line, sizeof(line), "Done in %.1f s: %d object(s), %d meshes, %s",
                      secs, static_cast<int>(rep.after.prims), static_cast<int>(rep.after.meshes),
                      HumanSize(rep.outputSizeBytes).c_str());
        const std::string dir = DirectoryOf(_outputPath);
        if (dir != ".") {
            usdtweak::SetAddonString(kAddonId, "lastDir", dir);
            usdtweak::PersistSettings();
        }
    } else {
        std::snprintf(line, sizeof(line), "Failed: %s", rep.error.c_str());
    }
    _resultLine = line;
}

void ExportPanel::DrawRun(const UsdStageRefPtr& stage, const std::vector<SdfPath>& targets) {
    std::string blocker = ExportBlocker(!targets.empty(), _outputPath);
    if (blocker.empty() && (!stage->GetRootLayer() || stage->GetRootLayer()->GetRealPath().empty())) {
        blocker = "Save the scene as a file first.";
    }
    if (!blocker.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", blocker.c_str());
        ImGui::BeginDisabled();
    }
    char label[64];
    std::snprintf(label, sizeof(label), "Export %d object(s)", static_cast<int>(targets.size()));
    if (ImGui::Button(targets.empty() ? "Export" : label, ImVec2(-1.0f, 0.0f))) {
        Run(stage, targets);
    }
    if (!blocker.empty()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !blocker.empty()) ImGui::SetTooltip("%s", blocker.c_str());

    if (!_resultLine.empty()) {
        ImGui::TextColored(_resultOk ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "%s", _resultLine.c_str());
        if (_resultOk) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BasenameOf(_outputPath).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _outputPath);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(_showReport ? "Hide details" : "Details")) _showReport = !_showReport;
        if (_showReport) {
            ImGui::BeginChild("report", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
            ImGui::TextUnformatted(_report.c_str());
            ImGui::EndChild();
        }
    }
}

void ExportPanel::Draw(const UsdStageRefPtr& stage, const std::vector<SdfPath>& selectionRoots) {
    const float top = ImGui::GetCursorPosY();
    const std::vector<SdfPath>& targets = _list.empty() ? selectionRoots : _list;

    ImGui::Separator();
    DrawList(selectionRoots);
    DrawPreset();
    DrawDestination(stage, targets);
    DrawAdvanced();
    DrawRun(stage, targets);

    _lastHeight = ImGui::GetCursorPosY() - top + ImGui::GetStyle().ItemSpacing.y;
}

}  // namespace usdprep_addon
