#include "ExportPanel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// 0 = .usdc, 1 = .abc, 2 = .obj - the order of the format list.
const char* FormatExtension(int format) { return format == 1 ? ".abc" : format == 2 ? ".obj" : ".usdc"; }

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
    if (name == "raw") return "Original (nothing changed)";
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
bool NativeSaveDialog(const std::string& suggestedName, int format, std::string& outPath) {
    if (!EnsureCom()) return false;
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    // Both formats Nuke was measured to read: USD, and .obj for its classic 3D.
    const COMDLG_FILTERSPEC filters[] = {{L"USD for Nuke (*.usdc)", L"*.usdc"},
                                         {L"Alembic for older Nuke (*.abc)", L"*.abc"},
                                         {L"OBJ for older Nuke (*.obj)", L"*.obj"}};
    dialog->SetFileTypes(3, filters);
    dialog->SetFileTypeIndex(format + 1);
    dialog->SetDefaultExtension(format == 1 ? L"abc" : format == 2 ? L"obj" : L"usdc");
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
    _materials = _recipe.materialPurpose == "preview" ? 0 : _recipe.materialPurpose == "full" ? 1 : 2;
    _animation = _recipe.animation == "all" ? 0 : _recipe.animation == "static" ? 2 : 1;
    _textureCap = _recipe.maxTextureSize >= 8192 ? 1
                  : _recipe.maxTextureSize >= 4096 ? 2
                  : _recipe.maxTextureSize >= 2048 ? 3
                  : _recipe.maxTextureSize >= 1024 ? 4
                  : _recipe.maxTextureSize > 0    ? 5
                                                  : 0;
    _geometry = _recipe.simplifyRatio <= 0.0  ? 0
                : _recipe.simplifyRatio > 0.35 ? 1
                : _recipe.simplifyRatio > 0.17 ? 2
                                               : 3;
    _includeLights =
        std::find(_recipe.dropTypes.begin(), _recipe.dropTypes.end(), "light") == _recipe.dropTypes.end();
    _dropGuideProxy = std::find(_recipe.dropPurposes.begin(), _recipe.dropPurposes.end(), "guide") !=
                          _recipe.dropPurposes.end() ||
                      std::find(_recipe.dropPurposes.begin(), _recipe.dropPurposes.end(), "proxy") !=
                          _recipe.dropPurposes.end();
    _stripRenderContexts = _recipe.stripRenderContexts;
    _stripUnusedMaterials = _recipe.stripUnusedMaterials;
    _stripCards = _recipe.stripDrawModeCards;
    _staticFrameSet = !std::isnan(_recipe.staticFrame);
    if (_staticFrameSet) _staticFrame = _recipe.staticFrame;

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
        const std::string suggested = dir + "/" + name + FormatExtension(_format);
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
        if (NativeSaveDialog(stem + FormatExtension(_format), _format, chosen)) {
            std::snprintf(_outputPath, sizeof(_outputPath), "%s", chosen.c_str());
            _pathEdited = true;
            const std::string ext = chosen.size() >= 4 ? chosen.substr(chosen.size() - 4) : std::string();
            _format = ext == ".abc" ? 1 : ext == ".obj" ? 2 : 0;
        }
    }
#else
    ImGui::BeginDisabled();
    ImGui::Button("Browse...");
    ImGui::EndDisabled();
#endif

    // Two formats, both measured in Nuke: USD for the current 3D system,
    // .obj for the classic one - the only 3D an older Nuke has.
    const char* formats[] = {
        "USD (.usdc) - Nuke's current 3D system, materials and animation",
        "Alembic (.abc) - older Nuke / classic 3D (ReadGeo): with animation",
        "OBJ (.obj) - older Nuke / classic 3D (ReadGeo): one still frame",
    };
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##format", &_format, formats, 3);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (_format == 0) {
        ImGui::TextWrapped("A .usdc file with a \"%s_textures\" folder next to it - keep the two together.",
                           OutputNameOf(_outputPath).c_str());
    } else {
        ImGui::TextWrapped("The file, its textures folder and \"%s.nk\": in Nuke, File > Insert Comp Nodes "
                           "brings the geometry with its textures wired in. %s",
                           OutputNameOf(_outputPath).c_str(),
                           _format == 1 ? "Animation is inside; set the Nuke project to the scene's frame rate."
                                        : "No animation in an .obj - it is a still of one frame.");
    }
    ImGui::PopStyleColor();
    // The extension follows the format - fixed up, but never while the artist is typing.
    if (!editingPath && _outputPath[0] != '\0') {
        const std::string synced = WithExtension(_outputPath, FormatExtension(_format));
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
    ImGui::Checkbox("Copy textures next to the file", &_relinkTextures);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copies every texture the objects use into a folder next to\n"
                          "the exported file and points the file at the copies.\n"
                          "Recommended.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Materials");
    ImGui::SameLine();
    const char* materialChoices[] = {
        "Light (preview) - small textures",
        "Full quality - hero textures",
        "Keep both",
    };
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##materials", &_materials, materialChoices, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Production assets often carry two materials per object: a heavy\n"
                          "one for final renders (4K UDIM textures) and a light one for\n"
                          "previews. Nuke is happy with the light one, and it is a fraction\n"
                          "of the size. UDIM tile sets, which Nuke cannot read, are stitched\n"
                          "into one texture each.");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Animation");
    ImGui::SameLine();
    char rangeLabel[96];
    std::snprintf(rangeLabel, sizeof(rangeLabel), "Shot range only (%g-%g)", _sceneStart, _sceneEnd);
    const char* animationChoices[] = {"Everything - every time sample", rangeLabel, "One frame - a still"};
    const float frameWidth = ImGui::CalcTextSize("00000000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(_animation == 2 ? -frameWidth - ImGui::GetStyle().ItemSpacing.x : -1.0f);
    ImGui::Combo("##animation", &_animation, animationChoices, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Simulations often carry frames from before the shot starts\n"
                          "(pre-roll); the shot range drops those. A still bakes one\n"
                          "frame and drops the animation altogether.");
    }
    if (_animation == 2) {
        ImGui::SameLine();
        if (!_staticFrameSet) _staticFrame = _sceneStart;
        ImGui::SetNextItemWidth(frameWidth);
        if (ImGui::InputDouble("##frame", &_staticFrame, 0.0, 0.0, "%g")) _staticFrameSet = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The frame to keep.");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Textures");
    ImGui::SameLine();
    const char* capChoices[] = {"Keep every texture as it is", "At most 8K (8192 px)", "At most 4K (4096 px)",
                                "At most 2K (2048 px)",         "At most 1K (1024 px)", "At most 512 px"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##texturecap", &_textureCap, capChoices, 6);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hero textures come in 4K and 8K; a comp never sees that much.\n"
                          "Larger textures are scaled down in the exported file only -\n"
                          "the originals stay as they are.");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Geometry");
    ImGui::SameLine();
    const char* geometryChoices[] = {"As it is", "Half the polygons", "A quarter of the polygons",
                                     "A tenth of the polygons"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##geometry", &_geometry, geometryChoices, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Decimates dense meshes, keeping UVs and normals as well as it can.\n"
                          "Never on by itself - a comp rarely needs it, and it changes the\n"
                          "shape. The result is triangles.");
    }

    // Off by default because it causes trouble in Nuke; switched on, what
    // Nuke cannot read is replaced by something it can, and reported.
    ImGui::Checkbox("Include lights", &_includeLights);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off by default: with a light in the file Nuke stops showing\n"
                          "surfaces unlit and the picture goes dark. Switched on, sphere,\n"
                          "disk and dome lights come along as lights; the types Nuke cannot\n"
                          "read (distant, rect, cylinder...) become axes of the same name,\n"
                          "in the same place, so you can rebuild them. The report lists them.");
    }

    // Every reduction the preset makes is a switch here, so the original
    // is always one click away.
    ImGui::Checkbox("Remove guide and proxy geometry", &_dropGuideProxy);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stand-in and helper geometry a production asset carries for its\n"
                          "own viewers. Nuke draws all of it, so the asset would show twice:\n"
                          "switched off, it stays in the file but hidden.");
    }
    ImGui::Checkbox("Remove renderer-only shader networks", &_stripRenderContexts);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Arnold, RenderMan and similar material outputs that Nuke cannot\n"
                          "render, with the shaders and textures only they use. Switched off\n"
                          "they stay - except MaterialX outputs, which make Nuke render the\n"
                          "material black.");
    }
    ImGui::Checkbox("Remove unused materials", &_stripUnusedMaterials);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Materials nothing in the export is bound to.");
    ImGui::Checkbox("Remove preview cards", &_stripCards);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Six small textures on a box that stand in for the asset in some\n"
                          "viewers. Nuke never draws them.");
    }
    std::string types;
    for (const std::string& t : _recipe.dropTypes) {
        if (t != "light") types += (types.empty() ? "" : ", ") + t;  // lights have their own switch
    }
    if (!types.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("This recipe also removes every: %s", types.c_str());
        ImGui::PopStyleColor();
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
    options.materialPurpose = _materials == 0 ? "preview" : _materials == 1 ? "full" : "all";
    options.animation = _animation == 0 ? "all" : _animation == 1 ? "range" : "static";
    if (_animation == 2) options.staticFrame = _staticFrame;
    static const int kCaps[] = {0, 8192, 4096, 2048, 1024, 512};
    options.maxTextureSize = kCaps[_textureCap];
    static const double kRatios[] = {0.0, 0.5, 0.25, 0.1};
    options.simplifyRatio = kRatios[_geometry];
    // the switches beat the recipe, both ways
    options.dropPurposes.erase(
        std::remove_if(options.dropPurposes.begin(), options.dropPurposes.end(),
                       [](const std::string& p) { return p == "guide" || p == "proxy"; }),
        options.dropPurposes.end());
    if (_dropGuideProxy) {
        options.dropPurposes.push_back("guide");
        options.dropPurposes.push_back("proxy");
    }
    options.dropTypes.erase(std::remove(options.dropTypes.begin(), options.dropTypes.end(), std::string("light")),
                            options.dropTypes.end());
    if (!_includeLights) options.dropTypes.push_back("light");
    options.stripRenderContexts = _stripRenderContexts;
    options.stripUnusedMaterials = _stripUnusedMaterials;
    options.stripDrawModeCards = _stripCards;
    // Not a switch in the panel: a UDIM set is something Nuke cannot read,
    // so only the "Original" recipe (nothing changed) leaves one alone.
    options.udimAtlas = _recipe.udimAtlas;
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
    _sceneStart = stage->GetStartTimeCode();
    _sceneEnd = stage->GetEndTimeCode();

    ImGui::Separator();
    DrawList(selectionRoots);
    DrawPreset();
    DrawDestination(stage, targets);
    DrawAdvanced();
    DrawRun(stage, targets);

    _lastHeight = ImGui::GetCursorPosY() - top + ImGui::GetStyle().ItemSpacing.y;
}

}  // namespace usdprep_addon
