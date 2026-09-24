#include "ExportPanel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <new>
#include <string>
#include <thread>

#include "addons/Api.h"
#include "Gui.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Extract.h>
#include <usdprep/Progress.h>

#include "NativeDialogs.h"
#include "OutputPath.h"
#include "SceneOpening.h"

namespace usdprep_addon {

namespace {

constexpr const char* kAddonId = "UsdPrep";

// 0 = .usdc, 1 = .abc, 2 = .obj - the order of the format list.
const char* FormatExtension(int format) { return format == 1 ? ".abc" : format == 2 ? ".obj" : ".usdc"; }

// The geometry slider: its stops, what each keeps of the polygons, and
// how it is labelled. 0 = as it is.
constexpr int kGeometrySteps = 6;
const double kGeometryRatios[kGeometrySteps] = {0.0, 0.5, 0.25, 0.1, 0.04, 0.01};
const char* const kGeometryLabels[kGeometrySteps] = {"As it is",
                                                     "1/2 of the polygons",
                                                     "1/4 of the polygons",
                                                     "1/10 of the polygons",
                                                     "1/25 of the polygons",
                                                     "1/100 of the polygons"};

// The stop nearest to a recipe's ratio, measured the way the stops are
// spaced: by how many times fewer polygons.
int GeometryStepFor(double ratio) {
    if (!(ratio > 0.0)) return 0;
    int best = 1;
    for (int step = 2; step < kGeometrySteps; ++step) {
        if (std::fabs(std::log(ratio / kGeometryRatios[step])) <
            std::fabs(std::log(ratio / kGeometryRatios[best]))) {
            best = step;
        }
    }
    return best;
}

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


}  // namespace

// ---------------------------------------------------------------------------
// the export in the background
// ---------------------------------------------------------------------------

struct ExportPanel::Job {
    usdprep::Progress progress;
    std::atomic<bool> finished{false};
    usdprep::Report report;
    std::thread worker;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::string outputPath;
    int objects = 0;

    ~Job() {
        // the program is closing in the middle of an export: stop it, then go
        progress.cancel = true;
        if (worker.joinable()) worker.join();
    }
};

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
    _geometry = GeometryStepFor(_recipe.simplifyRatio);
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
        if (dir.empty() && stage) {
            // next to the file the artist opened (a converted OBJ lives in a temporary folder)
            const std::string source = SourcePathOf(stage);
            if (!source.empty()) dir = DirectoryOf(source);
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
        ImGui::TextWrapped("The file, its textures folder and \"%s_%s.nk\": in Nuke, File > Insert Comp Nodes "
                           "brings the geometry with its textures wired in. %s",
                           OutputNameOf(_outputPath).c_str(), _format == 1 ? "abc" : "obj",
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
        ImGui::SetTooltip("Turns USD instancing into plain, standalone objects. Recommended:\n"
                          "Nuke reads instancing, but on a big scene (over a thousand\n"
                          "instances) its render stops with \"Too many open files\".\n"
                          "The file does not get any larger by it.");
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

    // Six stops on one slider: dragging from "as it is" to a hundredth
    // reads as "less and less", which a list of fractions does not.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Geometry");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    // the label is the slider's printf format: none of them may contain a percent sign
    ImGui::SliderInt("##geometry", &_geometry, 0, kGeometrySteps - 1, kGeometryLabels[_geometry],
                     ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Decimates dense meshes, keeping UVs and normals as well as it can.\n"
                          "Never on by itself - it changes the shape. The result is triangles;\n"
                          "meshes under 500 polygons are left as they are.\n\n"
                          "1/25 and 1/100 are for scans and photogrammetry: millions of\n"
                          "polygons where the comp needs a light stand-in.");
    }

    // Off by default because it causes trouble in Nuke; switched on, what
    // Nuke cannot read is replaced by something it can, and reported.
    ImGui::Checkbox("Include lights", &_includeLights);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off by default: with a light in the file Nuke stops showing\n"
                          "surfaces unlit and the picture goes dark. Switched on, distant,\n"
                          "sphere, disk and dome lights come along as lights; the types Nuke\n"
                          "cannot read (rect, cylinder...) become axes of the same name,\n"
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
    options.simplifyRatio = kGeometryRatios[_geometry];
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

    // The export reads the scene from its file, on a thread of its own: it
    // shares nothing with the scene on screen but the (read-only) layers.
    const std::string stagePath = stage->GetRootLayer()->GetRealPath();
    std::shared_ptr<Job> job = std::make_shared<Job>();
    job->outputPath = _outputPath;
    job->objects = static_cast<int>(targets.size());
    options.progress = &job->progress;
    Job* running = job.get();
    job->worker = std::thread([running, options, stagePath] {
        // an exception out of a thread ends the program: it becomes a failed export
        try {
            running->report = usdprep::ExtractPrims(stagePath, options);
        } catch (const std::bad_alloc&) {
            running->report.Fail("not enough memory for this export");
        } catch (const std::exception& e) {
            running->report.Fail(std::string("unexpected error: ") + e.what());
        } catch (...) {
            running->report.Fail("unexpected error");
        }
        running->finished = true;
    });
    _job = job;
    _resultLine.clear();
    _showReport = false;
}

void ExportPanel::Finish() {
    _job->worker.join();
    const usdprep::Report rep = _job->report;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - _job->started).count();
    const std::string outputPath = _job->outputPath;
    _job.reset();

    _resultOk = rep.ok;
    _resultCancelled = rep.error == "cancelled";
    _report = rep.ToText();
    char line[512];
    if (rep.ok) {
        std::snprintf(line, sizeof(line), "Done in %.1f s: %d object(s), %d meshes, %s",
                      secs, static_cast<int>(rep.after.prims), static_cast<int>(rep.after.meshes),
                      HumanSize(rep.outputSizeBytes).c_str());
        const std::string dir = DirectoryOf(outputPath);
        if (dir != ".") {
            usdtweak::SetAddonString(kAddonId, "lastDir", dir);
            usdtweak::PersistSettings();
        }
    } else if (rep.error == "cancelled") {
        std::snprintf(line, sizeof(line), "Export stopped - nothing was written.");
    } else {
        std::snprintf(line, sizeof(line), "Failed: %s", rep.error.c_str());
    }
    _resultLine = line;
}

// Where the green button was: a bar that fills as the export goes, the step
// it is on, and a light that keeps sweeping across - so that even a long
// single step (flattening a big set) never looks like a frozen program.
void ExportPanel::DrawProgress() {
    Job& job = *_job;
    const float fraction = std::min(std::max(static_cast<float>(job.progress.fraction), 0.0f), 1.0f);
    const char* step = job.progress.step;
    const int seconds = static_cast<int>(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count());

    char overlay[160];
    std::snprintf(overlay, sizeof(overlay), "%d %%  -  %s", static_cast<int>(fraction * 100.0f + 0.5f),
                  (step && *step) ? step : "starting");
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.FramePadding.y * 2.4f;  // as tall as the button
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kExportColor);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, height), overlay);
    ImGui::PopStyleColor();

    const ImVec2 low = ImGui::GetItemRectMin();
    const ImVec2 high = ImGui::GetItemRectMax();
    const float width = high.x - low.x;
    const float phase = static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1.4)) - 0.2f;  // -0.2 .. 1.2
    const float centre = low.x + width * phase;
    const float half = width * 0.12f;
    const ImU32 clear = IM_COL32(255, 255, 255, 0);
    const ImU32 light = IM_COL32(255, 255, 255, 60);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(low, high, true);
    draw->AddRectFilledMultiColor(ImVec2(centre - half, low.y), ImVec2(centre, high.y), clear, light, light, clear);
    draw->AddRectFilledMultiColor(ImVec2(centre, low.y), ImVec2(centre + half, high.y), light, clear, clear, light);
    draw->PopClipRect();

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "Exporting %d object(s) to %s  -  %d:%02d", job.objects,
                       BasenameOf(job.outputPath).c_str(), seconds / 60, seconds % 60);
    ImGui::SameLine();
    if (job.progress.cancel) {
        ImGui::TextDisabled("stopping...");
    } else if (ImGui::SmallButton("Cancel")) {
        job.progress.cancel = true;
    }
    ImGui::TextDisabled("The program is working - a big scene can take a few minutes.");
}

void ExportPanel::DrawRun(const UsdStageRefPtr& stage, const std::vector<SdfPath>& targets) {
    if (_job) {
        DrawProgress();
        return;
    }
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
        // done: green; stopped by the artist: plain; failed: red
        const ImVec4 color = _resultOk          ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
                             : _resultCancelled ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                : ImVec4(1.0f, 0.5f, 0.4f, 1.0f);
        ImGui::TextColored(color, "%s", _resultLine.c_str());
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

    if (_job && _job->finished) Finish();

    ImGui::Separator();
    // while an export runs, its settings are the ones it started with
    const bool busy = _job != nullptr;
    if (busy) ImGui::BeginDisabled();
    DrawList(selectionRoots);
    DrawPreset();
    DrawDestination(stage, targets);
    DrawAdvanced();
    if (busy) ImGui::EndDisabled();
    DrawRun(stage, targets);

    _lastHeight = ImGui::GetCursorPosY() - top + ImGui::GetStyle().ItemSpacing.y;
}

}  // namespace usdprep_addon
