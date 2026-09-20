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

// The three buttons that matter are found before anything is read:
// bigger than the rest, and each in its own colour.
const ImVec4 kAddColor(0.16f, 0.44f, 0.62f, 1.0f);     // into the list: blue
const ImVec4 kClearColor(0.48f, 0.24f, 0.24f, 1.0f);   // out of the list: brown-red
const ImVec4 kExportColor(0.18f, 0.55f, 0.28f, 1.0f);  // go: green
const ImVec4 kListBackground(0.12f, 0.17f, 0.23f, 1.0f);
const ImVec4 kSelectedText(1.0f, 0.85f, 0.2f, 1.0f);   // same yellow as the tree

bool BigButton(const char* label, const ImVec4& color, const ImVec2& size) {
    const ImVec4 hover(std::min(color.x * 1.3f, 1.0f), std::min(color.y * 1.3f, 1.0f),
                       std::min(color.z * 1.3f, 1.0f), 1.0f);
    const ImVec4 active(color.x * 0.8f, color.y * 0.8f, color.z * 0.8f, 1.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x * 2.0f, style.FramePadding.y * 2.2f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return pressed;
}

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
    const ImGuiStyle& style = ImGui::GetStyle();

    // ----- the two list buttons, side by side above the list -----------
    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    const bool canAdd = !selectionRoots.empty();
    if (!canAdd) ImGui::BeginDisabled();
    if (BigButton("Add to export", kAddColor, ImVec2(half, 0.0f))) {
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
    if (_list.empty()) ImGui::BeginDisabled();
    if (BigButton("Clear list", kClearColor, ImVec2(half, 0.0f))) _list.clear();
    if (_list.empty()) ImGui::EndDisabled();

    // ----- what Export will take, in a frame of its own ----------------
    // The list when there is one; otherwise the selection, so the frame
    // always answers "what goes out if I press Export now".
    const bool fromList = !_list.empty();
    const std::vector<SdfPath>& shown = fromList ? _list : selectionRoots;
    // Sized by its content; only a list long enough to crowd out the
    // tree gets a scrollbar instead of more height.
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float rows = std::max<float>(static_cast<float>(shown.size()), 1.0f);
    const float frameHeight = std::min(rowHeight * (rows + 1.0f) + style.WindowPadding.y * 2.0f,
                                       ImGui::GetWindowHeight() * 0.45f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kListBackground);
    ImGui::BeginChild("export-list", ImVec2(0.0f, frameHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (fromList) {
        ImGui::Text("To export: %d object(s)", static_cast<int>(shown.size()));
    } else if (shown.empty()) {
        ImGui::TextDisabled("Nothing to export yet - pick an object.");
    } else {
        ImGui::Text("To export (the selection): %d object(s)", static_cast<int>(shown.size()));
    }
    for (size_t i = 0; i < shown.size(); /*advanced in loop*/) {
        const SdfPath path = shown[i];
        ImGui::PushID(static_cast<int>(i));
        bool remove = false;
        if (fromList) {
            remove = ImGui::SmallButton("x");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the export list");
            ImGui::SameLine();
        }
        // An entry that is the current selection is yellow, like in the
        // tree; a click makes any entry the selection and shows it.
        const bool isSelected =
            std::find(selectionRoots.begin(), selectionRoots.end(), path) != selectionRoots.end();
        ImGui::PushStyleColor(ImGuiCol_Text, isSelected ? kSelectedText
                                                        : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::Selectable(path.GetName().c_str(), false) && onPick) onPick(path);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick to select it in the tree and the 3D view.", path.GetText());
        ImGui::PopID();
        if (remove) {
            _list.erase(_list.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Room under the frame: what goes out is one thing, how it goes out
    // is another.
    ImGui::Dummy(ImVec2(0.0f, rowHeight * 0.5f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, rowHeight * 0.3f));
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
    if (BigButton(targets.empty() ? "Export" : label, kExportColor, ImVec2(-1.0f, 0.0f))) {
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
