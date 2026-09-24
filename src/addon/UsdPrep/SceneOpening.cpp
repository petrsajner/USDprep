#include "SceneOpening.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "addons/Api.h"
#include "Gui.h"

#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <usdprep/Import.h>

#include "NativeDialogs.h"
#include "OutputPath.h"

namespace usdprep_addon {

namespace {

namespace fs = std::filesystem;

fs::path U8(const std::string& path) { return fs::u8path(path); }

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool IsUsdFile(const std::string& path) {
    const std::string ext = Lower(U8(path).extension().u8string());
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

std::string NameOf(const std::string& path) { return U8(path).filename().u8string(); }

// ------------------------------------------------------------ the conversions

// %TEMP%\USDprep\import\<name>-<signature hash>\<name>.usdc, next to it the
// signature of the source it was made from and the notes of the import.
fs::path ImportRoot() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / "USDprep" / "import";
}

// A conversion is good while the source is the same file, of the same
// size, written at the same time.
std::string SignatureOf(const std::string& source) {
    std::error_code ec;
    const fs::path path = fs::absolute(U8(source), ec);
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
    return path.u8string() + "|" + std::to_string(size) + "|" + std::to_string(time);
}

std::string HashOf(const std::string& text) {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer, 12);
}

struct Conversion {
    fs::path dir;
    fs::path usd;
    fs::path signature() const { return dir / "source.txt"; }
    fs::path notes() const { return dir / "notes.txt"; }
};

// Several places per source: an earlier conversion may still be open (and
// locked) in another USDprep while this one needs a fresh one.
Conversion ConversionFor(const std::string& source, const std::string& signature, int place) {
    std::string name;
    for (const char c : U8(source).stem().u8string()) {
        name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    Conversion conversion;
    conversion.dir = ImportRoot() / (name + "-" + HashOf(signature) + (place ? "-" + std::to_string(place) : ""));
    conversion.usd = conversion.dir / U8(U8(source).stem().u8string() + ".usdc");
    return conversion;
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    return file.good();
}

// A finished conversion of exactly this source, if there is one.
bool FindConversion(const std::string& source, const std::string& signature, Conversion* found) {
    constexpr int kPlaces = 8;
    for (int place = 0; place < kPlaces; ++place) {
        const Conversion conversion = ConversionFor(source, signature, place);
        std::error_code ec;
        if (fs::is_regular_file(conversion.usd, ec) && fs::is_regular_file(conversion.notes(), ec) &&
            ReadText(conversion.signature()) == signature) {
            *found = conversion;
            return true;
        }
    }
    return false;
}

// The report as notes for the artist: warnings first, one per line.
std::string NotesOf(const usdprep::Report& report) {
    std::string warnings, infos;
    for (const usdprep::ReportEntry& e : report.entries) {
        if (e.severity == usdprep::ReportEntry::Severity::Info) {
            infos += "info: " + e.detail + "\n";
        } else {
            warnings += "warning: " + e.detail + "\n";
        }
    }
    return warnings + infos;
}

struct ImportJob {
    std::string source;
    std::string signature;
    Conversion conversion;
    usdprep::ImportProgress progress;
    std::atomic<bool> finished{false};
    usdprep::Report report;
    std::thread worker;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    ~ImportJob() {
        // the program is closing in the middle of a conversion: stop it, then go
        progress.cancel = true;
        if (worker.joinable()) worker.join();
    }
};

std::unique_ptr<ImportJob>& Job() {
    static std::unique_ptr<ImportJob> job;
    return job;
}

// One message at a time, shown until OK.
std::string& Notice() {
    static std::string notice;
    return notice;
}

// A stage that was asked to open: if the next frame still shows the one
// before, USD could not read the file.
struct PendingOpen {
    bool active = false;
    std::string path;
    pxr::UsdStageRefPtr before;
};

PendingOpen& Pending() {
    static PendingOpen pending;
    return pending;
}

void Open(std::string path) { ExecuteAfterDraw<EditorOpenStage>(path); }

// A scan converted at survey coordinates keeps its place through a
// double-precision translate on its root. The 3D view draws in floats:
// out there its polygons snap to a coarse grid and it looks broken. So the
// view shows it at the origin - an opinion in the session layer, which is
// never saved and never exported (the export reads the file itself) - and
// the panel says where it really is.
struct Displaced {
    pxr::UsdStageRefPtr stage;
    pxr::GfVec3d position{0.0};
};

Displaced& DisplacedStage() {
    static Displaced displaced;
    return displaced;
}

void ShowAtOrigin(const pxr::UsdStageRefPtr& stage) {
    Displaced& displaced = DisplacedStage();
    if (displaced.stage == stage) return;
    displaced = Displaced();
    if (!stage || !stage->GetRootLayer()) return;
    const pxr::VtDictionary data = stage->GetRootLayer()->GetCustomLayerData();
    if (data.count(usdprep::kImportedFromKey) == 0) return;
    const pxr::UsdGeomXformable root(stage->GetDefaultPrim());
    if (!root) return;
    bool resets = false;
    for (const pxr::UsdGeomXformOp& op : root.GetOrderedXformOps(&resets)) {
        pxr::GfVec3d position(0.0);
        if (op.GetOpType() != pxr::UsdGeomXformOp::TypeTranslate || !op.Get(&position)) continue;
        if (position == pxr::GfVec3d(0.0)) continue;
        pxr::UsdEditContext session(stage, stage->GetSessionLayer());
        op.Set(pxr::GfVec3d(0.0));
        displaced.stage = stage;
        displaced.position = position;
        usdtweak::FrameCameraOnScene();  // the view was framed on the far-away place
        return;
    }
    displaced.stage = stage;  // looked at: nothing to move
}

void StartImport(const std::string& source, const std::string& signature) {
    std::unique_ptr<ImportJob>& job = Job();
    if (job) {
        if (job->source != source) {
            Notice() = "Still converting " + NameOf(job->source) + ". Open " + NameOf(source) +
                       " again when it is done.";
        }
        return;
    }
    Conversion conversion;
    for (int place = 0; place < 8; ++place) {
        conversion = ConversionFor(source, signature, place);
        std::error_code ec;
        fs::remove_all(conversion.dir, ec);
        if (!fs::exists(conversion.dir, ec)) break;  // else it is in use: the next place
    }
    std::error_code ec;
    fs::create_directories(conversion.dir, ec);

    job = std::make_unique<ImportJob>();
    job->source = source;
    job->signature = signature;
    job->conversion = conversion;
    ImportJob* running = job.get();
    job->worker = std::thread([running] {
        // an exception out of a thread ends the program: it becomes a failed conversion
        try {
            running->report =
                usdprep::ImportToUsd(running->source, running->conversion.usd.u8string(), &running->progress);
        } catch (const std::bad_alloc&) {
            running->report.Fail("not enough memory for this file");
        } catch (const std::exception& e) {
            running->report.Fail(std::string("unexpected error: ") + e.what());
        } catch (...) {
            running->report.Fail("unexpected error");
        }
        running->finished = true;
    });
}

// ------------------------------------------------------------------ windows

// A small window in the middle of the program, in front of everything.
bool BeginCentered(const char* title, float widthInFonts) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * widthInFonts, 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings;
    const bool open = ImGui::Begin(title, nullptr, kFlags);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    return open;
}

void DrawProgress(ImportJob& job) {
    if (BeginCentered("Converting to USD###UsdPrepImport", 30.0f)) {
        const float fraction = job.progress.fraction;
        const int seconds = static_cast<int>(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count());
        ImGui::TextWrapped("%s", NameOf(job.source).c_str());
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s  -  %d s", fraction < 0.8f ? "reading the file" : "writing the USD file", seconds);
        ImGui::TextDisabled("The original file is only read, never changed.");
        if (job.progress.cancel) {
            ImGui::TextDisabled("Stopping...");
        } else if (ImGui::Button("Cancel")) {
            job.progress.cancel = true;
        }
    }
    ImGui::End();
}

void DrawNotice() {
    std::string& notice = Notice();
    if (notice.empty()) return;
    if (BeginCentered("USDprep###UsdPrepNotice", 30.0f)) {
        ImGui::TextWrapped("%s", notice.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f))) notice.clear();
    }
    ImGui::End();
}

}  // namespace

// ------------------------------------------------------------------- hooks

bool OnOpenDialog() {
    if (!NativeDialogsAvailable()) return false;  // usdtweak's own browser, then
    std::string path;
    if (NativeOpenSceneDialog(path)) Open(path);
    return true;
}

namespace {

bool CheckAndPrepare(std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(U8(path), ec)) {
        Notice() = "Cannot open " + path + "\n\nThe file is not there, or this computer cannot reach it.";
        return false;
    }
    if (usdprep::IsImportable(path)) {
        const std::string signature = SignatureOf(path);
        Conversion done;
        if (FindConversion(path, signature, &done)) {
            path = done.usd.u8string();  // converted before, nothing changed since
        } else {
            StartImport(path, signature);
            return false;  // opened when the conversion is done
        }
    }
    Pending() = {true, path, usdtweak::GetCurrentStage()};
    return true;
}

}  // namespace

bool OnOpenStage(std::string& path) {
    // Called from inside usdtweak: an error here is a message, never an exception.
    try {
        return CheckAndPrepare(path);
    } catch (const std::exception& e) {
        Notice() = "Cannot open " + path + "\n\n" + e.what();
    } catch (...) {
        Notice() = "Cannot open " + path;
    }
    return false;
}

bool OnDropFile(const std::string& path) {
    if (!IsUsdFile(path) && !usdprep::IsImportable(path)) return false;
    Open(path);
    return true;
}

void OnFrame() {
    ShowAtOrigin(usdtweak::GetCurrentStage());

    PendingOpen& pending = Pending();
    if (pending.active) {
        pending.active = false;
        if (usdtweak::GetCurrentStage() == pending.before) {
            Notice() = "USD could not read " + NameOf(pending.path) +
                       "\n\nThe file may be damaged, still being written, or not a USD scene.";
        }
        pending.before = nullptr;
    }

    std::unique_ptr<ImportJob>& job = Job();
    if (job && job->finished) {
        job->worker.join();
        const usdprep::Report report = job->report;
        const std::string source = job->source;
        const Conversion conversion = job->conversion;
        const std::string signature = job->signature;
        job.reset();
        std::error_code ec;
        if (report.ok && WriteText(conversion.notes(), NotesOf(report)) &&
            WriteText(conversion.signature(), signature)) {
            Open(source);  // finds the conversion now
        } else {
            fs::remove_all(conversion.dir, ec);
            if (report.error != "cancelled") {
                Notice() = "Cannot convert " + NameOf(source) + "\n\n" +
                           (report.error.empty() ? std::string("The temporary folder cannot be written.") : report.error);
            }
        }
    } else if (job) {
        DrawProgress(*job);
    }
    DrawNotice();
}

void ForgetOldImports() {
    std::error_code ec;
    for (fs::directory_iterator it(ImportRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code removeError;
        fs::remove_all(it->path(), removeError);  // one still open elsewhere is locked and stays
    }
}

// ------------------------------------------------------------------ queries

bool ShownAtOrigin(const pxr::UsdStageRefPtr& stage, pxr::GfVec3d* position) {
    const Displaced& displaced = DisplacedStage();
    if (!stage || displaced.stage != stage || displaced.position == pxr::GfVec3d(0.0)) return false;
    if (position) *position = displaced.position;
    return true;
}

bool IsImported(const pxr::UsdStageRefPtr& stage) {
    if (!stage || !stage->GetRootLayer()) return false;
    return stage->GetRootLayer()->GetCustomLayerData().count(usdprep::kImportedFromKey) > 0;
}

std::string SourcePathOf(const pxr::UsdStageRefPtr& stage) {
    if (!stage || !stage->GetRootLayer()) return {};
    const pxr::VtDictionary data = stage->GetRootLayer()->GetCustomLayerData();
    const auto found = data.find(usdprep::kImportedFromKey);
    if (found != data.end() && found->second.IsHolding<std::string>()) {
        return found->second.UncheckedGet<std::string>();
    }
    return stage->GetRootLayer()->GetRealPath();
}

const std::string& ImportNotesOf(const pxr::UsdStageRefPtr& stage) {
    static pxr::UsdStageRefPtr known;
    static std::string notes;
    if (known != stage) {
        known = stage;
        notes.clear();
        if (IsImported(stage)) {
            notes = ReadText(U8(stage->GetRootLayer()->GetRealPath()).parent_path() / "notes.txt");
        }
    }
    return notes;
}

}  // namespace usdprep_addon
