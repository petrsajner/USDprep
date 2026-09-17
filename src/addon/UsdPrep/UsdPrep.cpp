// UsdPrep — the "prepare for Nuke" addon.
//
// Primary workflow (the visual path): open a stage, pick the objects to
// export — by clicking in the viewport/outliner OR by checking them in the
// scene tree in this panel — then hit "Export selection". usdprep-core
// does the heavy lifting (mask -> flatten -> de-instance -> defaultPrim ->
// package).
//
// Selection model: the editor's stage selection is the single source of
// truth. The tree checkboxes ARE that selection (a checked node exports
// its whole subtree); every change is applied through the editor's command
// queue after the frame, so viewport, outliner and this panel always agree.
//
// UX rule: this panel speaks to comp artists, not USD engineers.

#include "addons/Api.h"
#include "Editor.h"
#include "Gui.h"
#include "Selection.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>

#include <usdprep/Extract.h>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace {

constexpr const char* kAddonId = "UsdPrep";

// ---------------------------------------------------------------------------
// small path/string helpers
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

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
    return it != haystack.end();
}

std::vector<SdfPath> ToSortedPaths(const std::set<std::string>& pathStrs) {
    std::vector<SdfPath> paths;
    paths.reserve(pathStrs.size());
    for (const std::string& s : pathStrs) paths.emplace_back(s);
    return paths;  // std::set iterates sorted already
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
// selection helpers — the editor selection is the single source of truth
// ---------------------------------------------------------------------------

// One-shot reveal state: tree nodes that must be forced open this frame
// and the row to scroll to (filled when a pick arrives from outside).
std::set<std::string>& PendingOpen() {
    static std::set<std::string> s;
    return s;
}

std::string& RevealTarget() {
    static std::string s;
    return s;
}

UsdStageRefPtr& LastStage() {
    static UsdStageRefPtr stage;
    return stage;
}

std::vector<SdfPath>& LastSyncedSelection() {
    static std::vector<SdfPath> sel;
    return sel;
}

bool IsAncestorInSet(const std::set<std::string>& sel, const std::string& pathStr) {
    for (SdfPath p(pathStr); p.GetPathElementCount() >= 1; p = p.GetParentPath()) {
        if (sel.count(p.GetAsString())) return true;
    }
    return false;
}

// Export roots: selected prims without a selected ancestor.
std::vector<SdfPath> ExportRoots(const std::set<std::string>& sel) {
    std::set<std::string> roots;
    for (const std::string& pathStr : sel) {
        bool covered = false;
        for (SdfPath p = SdfPath(pathStr).GetParentPath();
             p.GetPathElementCount() >= 1; p = p.GetParentPath()) {
            if (sel.count(p.GetAsString())) {
                covered = true;
                break;
            }
        }
        if (!covered) roots.insert(pathStr);
    }
    return ToSortedPaths(roots);
}

// Apply a new selection, after the current frame finishes. Mutating editor
// state mid-draw is not allowed — route through the command queue.
void ApplySelectionDeferred(const UsdStageRefPtr& stage,
                            const std::vector<SdfPath>& paths) {
    const auto apply = [](UsdStageRefPtr, const std::vector<SdfPath>& paths) {
        if (paths.empty()) {
            if (Editor* editor = usdtweak::GetEditor()) {
                // no Clear API on the addon surface yet (upstream request)
                editor->GetSelection().Clear(usdtweak::GetCurrentStage());
            }
            return;
        }
        usdtweak::SetStagePathSelection(paths.front());
        for (size_t i = 1; i < paths.size(); ++i) {
            usdtweak::AddStagePathSelection(paths[i]);
        }
    };
    ExecuteAfterDraw(apply, stage, paths);
}

std::string SuggestOutputPath(const UsdStageRefPtr& stage, const SdfPath& firstPrim) {
    std::string dir = usdtweak::GetAddonString(kAddonId, "lastDir", "");
    if (dir.empty() && stage && stage->GetRootLayer()) {
        const std::string realPath = stage->GetRootLayer()->GetRealPath();
        if (!realPath.empty()) dir = DirectoryOf(realPath);
    }
    if (dir.empty()) dir = ".";
    std::string name = firstPrim.IsEmpty() ? std::string("asset") : firstPrim.GetName();
    if (name.empty()) name = "asset";
    return dir + "/" + name + ".usdz";
}

// One row of the tree (or of the flat search result list).
void DrawPrimRow(const UsdPrim& prim, bool flat, const std::set<std::string>& sel,
                 const UsdStageRefPtr& stage) {
    const std::string pathStr = prim.GetPath().GetAsString();

    ImGui::PushID(pathStr.c_str());
    const bool inherited = IsAncestorInSet(sel, pathStr);
    const bool selected = sel.count(pathStr) > 0;
    bool checked = inherited || selected;
    if (inherited) ImGui::BeginDisabled();
    if (ImGui::Checkbox("##inc", &checked)) {
        if (checked) {
            // add this prim on top of the current export roots
            std::set<std::string> next(sel.begin(), sel.end());
            next.insert(pathStr);
            // drop descendants that are now covered anyway (cosmetic)
            ApplySelectionDeferred(stage, ToSortedPaths(next));
        } else {
            // remove this prim and everything beneath it
            const SdfPath removed(pathStr);
            std::set<std::string> next;
            for (const std::string& candidate : sel) {
                if (!SdfPath(candidate).HasPrefix(removed)) next.insert(candidate);
            }
            ApplySelectionDeferred(stage, ToSortedPaths(next));
        }
    }
    if (inherited) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", pathStr.c_str());
    }

    ImGui::SameLine();
    std::vector<UsdPrim> children;
    if (!flat) {
        const auto siblings = prim.GetAllChildren();
        children.assign(siblings.begin(), siblings.end());
    }
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selected || inherited) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!flat && PendingOpen().count(pathStr)) {
        ImGui::SetNextItemOpen(true);
    }
    const bool open =
        ImGui::TreeNodeEx(prim.GetName().GetString().c_str(), flags) && !children.empty();
    if (pathStr == RevealTarget()) {
        ImGui::SetScrollHereY(0.25f);
    }
    ImGui::SameLine();
    const std::string typeName = prim.GetTypeName().GetString();
    if (!typeName.empty()) {
        ImGui::TextDisabled("%s", typeName.c_str());
    }
    if (open) {
        for (const UsdPrim& child : children) {
            DrawPrimRow(child, flat, sel, stage);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawPrepAddon() {
    const UsdStageRefPtr stage = usdtweak::GetCurrentStage();
    if (!stage) {
        ImGui::TextUnformatted("Open a USD stage, select objects, then export.");
        return;
    }

    if (LastStage() != stage) {
        LastStage() = stage;
        LastSyncedSelection().clear();
        PendingOpen().clear();
        RevealTarget().clear();
    }

    // The selection IS the check state. Normalize to prim paths.
    const std::vector<SdfPath> selection = usdtweak::GetSelection().GetSelectedPaths(stage);
    std::set<std::string> sel;
    for (const SdfPath& p : selection) {
        const SdfPath prim = p.IsPrimPath() ? p : p.GetPrimPath();
        if (prim.IsPrimPath() && !prim.IsAbsoluteRootPath()) {
            sel.insert(prim.GetAsString());
        }
    }

    // Reveal newly picked objects in the tree (expand + scroll), one-shot.
    if (selection != LastSyncedSelection()) {
        for (const SdfPath& p : selection) {
            const bool known = std::find(LastSyncedSelection().begin(),
                                         LastSyncedSelection().end(), p) !=
                               LastSyncedSelection().end();
            if (known) continue;
            const SdfPath prim = p.IsPrimPath() ? p : p.GetPrimPath();
            if (!prim.IsPrimPath() || prim.IsAbsoluteRootPath()) continue;
            for (SdfPath a = prim; a.GetPathElementCount() >= 1; a = a.GetParentPath()) {
                PendingOpen().insert(a.GetAsString());
            }
            RevealTarget() = prim.GetAsString();
        }
        LastSyncedSelection() = selection;
    }

    const std::vector<SdfPath> roots = ExportRoots(sel);

    // ----- what gets exported: scene tree with checkboxes ---------------
    ImGui::Text("Objects to export: %d", static_cast<int>(roots.size()));
    if (!roots.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        const size_t shown = std::min<size_t>(roots.size(), 12);
        for (size_t i = 0; i < shown; ++i) {
            ImGui::BulletText("%s", roots[i].GetAsString().c_str());
        }
        if (roots.size() > shown) {
            ImGui::Text("... and %d more", static_cast<int>(roots.size() - shown));
        }
        ImGui::EndTooltip();
    }

    static char filter[128] = "";
    ImGui::InputTextWithHint("##filter", "Search objects...", filter, sizeof(filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear##clearSel")) {
        ApplySelectionDeferred(stage, {});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Deselect everything (same as clicking empty space\nin the 3D view).");
    }

    ImGui::BeginChild("tree", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f),
                      ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);
    if (filter[0] != '\0') {
        size_t shown = 0;
        for (const UsdPrim& prim :
             UsdPrimRange(stage->GetPseudoRoot(),
                          UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            if (prim.IsPseudoRoot()) continue;
            if (ContainsCaseInsensitive(prim.GetName(), filter)) {
                DrawPrimRow(prim, /*flat=*/true, sel, stage);
                if (++shown >= 200) {
                    ImGui::TextDisabled("... more than 200 matches, keep typing");
                    break;
                }
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("    (no objects match '%s')", filter);
        }
    } else {
        for (const UsdPrim& child : stage->GetPseudoRoot().GetAllChildren()) {
            DrawPrimRow(child, /*flat=*/false, sel, stage);
        }
    }
    ImGui::EndChild();
    // The reveal was applied this frame — one-shot.
    PendingOpen().clear();
    RevealTarget().clear();

    ImGui::Separator();

    // ----- where it goes: path + native save dialog -------------------
    static char outputPath[512] = "";
    if (outputPath[0] == '\0' && !roots.empty()) {
        const std::string suggested = SuggestOutputPath(stage, roots.front());
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
        const std::string suggestion =
            HasUsdExtension(current)
                ? BasenameOf(current)
                : BasenameOf(current) + std::string(format == 0 ? ".usdz" : ".usdc");
        if (NativeSaveDialog(suggestion, format == 0, chosen)) {
            std::snprintf(outputPath, sizeof(outputPath), "%s", chosen.c_str());
            format = (chosen.size() > 5 &&
                      chosen.compare(chosen.size() - 5, 5, ".usdz") != 0)
                         ? 1
                         : 0;
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
    const bool canExport = !roots.empty() && outputPath[0] != '\0';
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export selection", ImVec2(-1.0f, 0.0f))) {
        usdprep::ExtractOptions options;
        options.primPaths.reserve(roots.size());
        for (const SdfPath& p : roots) {
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
    if (ImGui::IsItemHovered() && !canExport) {
        ImGui::SetTooltip(
            "Check at least one object in the tree above\n(or click objects in the 3D view).");
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
