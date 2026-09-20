// Non-usdz outputs must be as portable as a .usdz: the textures travel
// next to the layer and the asset paths point at the copies.
#include <cstdio>
#include <filesystem>
#include <string>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Extract.h>
#include <usdprep/StageInfo.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;

// Every asset path authored in the output, in stage order.
std::vector<std::string> AssetPaths(const std::string& stagePath) {
    std::vector<std::string> paths;
    const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(stagePath);
    if (!stage) return paths;
    for (const pxr::UsdPrim& prim : stage->Traverse()) {
        for (const pxr::UsdAttribute& attr : prim.GetAttributes()) {
            if (attr.GetTypeName() != pxr::SdfValueTypeNames->Asset) continue;
            pxr::SdfAssetPath value;
            if (attr.Get(&value) && !value.GetAssetPath().empty()) {
                paths.push_back(value.GetAssetPath());
            }
        }
    }
    return paths;
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/textured_scene.usda";
    const fs::path outDir = fs::current_path() / "relink_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- default: textures are copied next to the output and repointed ---
    {
        usdprep::ExtractOptions options;
        options.udimAtlas = false;  // the set-stays-a-set path; the atlas has its own test
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "panel.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(fs::exists(options.outputPath));
        // USD's localizer grumbles about every UDIM template it then
        // expands; that noise must not reach the report as a warning
        for (const usdprep::ReportEntry& entry : rep.entries) {
            CHECK(entry.action != "usd");
            CHECK(entry.detail.find("Failed to resolve") == std::string::npos);
        }

        const fs::path sidecar = outDir / "panel_textures";
        CHECK(fs::is_directory(sidecar));
        size_t copied = 0;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(sidecar, ec)) {
            if (entry.is_regular_file(ec)) ++copied;
        }
        CHECK(copied == 3);  // checker.png + both UDIM tiles

        const std::vector<std::string> paths = AssetPaths(options.outputPath);
        CHECK(paths.size() == 2);
        bool udimKept = false;
        for (const std::string& path : paths) {
            CHECK(path.rfind("panel_textures/", 0) == 0);
            CHECK(!fs::path(path).is_absolute());
            if (path.find("<UDIM>") != std::string::npos) udimKept = true;
            // the template itself is not a file; its tiles are
            if (path.find("<UDIM>") == std::string::npos) {
                CHECK(fs::exists(outDir / path));
            }
        }
        CHECK(udimKept);  // the tile token survives, one path for the set
        CHECK(fs::exists(sidecar / "1" / "tile.1001.png") ||
              fs::exists(sidecar / "0" / "tile.1001.png"));
    }

    // --- opt out: paths stay as they were, no sidecar folder ---
    {
        usdprep::ExtractOptions options;
        options.udimAtlas = false;  // the set-stays-a-set path; the atlas has its own test
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "panel_raw.usdc").string();
        options.relinkTextures = false;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(!fs::exists(outDir / "panel_raw_textures"));
        for (const std::string& path : AssetPaths(options.outputPath)) {
            CHECK(path.rfind("panel_raw_textures/", 0) != 0);
        }
    }

    // --- a scene without textures gets no sidecar folder at all ---
    {
        usdprep::ExtractOptions options;
        options.udimAtlas = false;  // the set-stays-a-set path; the atlas has its own test
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "plain.usdc").string();
        const usdprep::Report rep =
            usdprep::ExtractPrims(FIXTURE_DIR "/simple_scene.usda", options);
        CHECK(rep.ok);
        CHECK(!fs::exists(outDir / "plain_textures"));
    }

    // --- textures that are not on this machine are left out, named, and
    //     do not stop the export (USD's packager would give up on them) ---
    {
        const std::string partial = FIXTURE_DIR "/missing_textures.usda";
        for (const char* ext : {".usdz", ".usdc"}) {
            usdprep::ExtractOptions options;
            options.udimAtlas = false;  // the set-stays-a-set path; the atlas has its own test
            options.primPaths = {"/Root"};
            options.outputPath = (outDir / (std::string("partial") + ext)).string();
            const usdprep::Report rep = usdprep::ExtractPrims(partial, options);
            CHECK(rep.ok);
            CHECK(fs::exists(options.outputPath));
            CHECK(rep.after.textureRefs == 2);  // checker + the tile set survive
            bool named = false;
            for (const usdprep::ReportEntry& entry : rep.entries) {
                if (entry.severity == usdprep::ReportEntry::Severity::Warning &&
                    entry.detail.find("not_delivered.png") != std::string::npos &&
                    entry.detail.find("2 texture") != std::string::npos) {
                    named = true;
                }
            }
            CHECK(named);
        }
        const std::vector<std::string> paths = AssetPaths((outDir / "partial.usdc").string());
        CHECK(paths.size() == 2);
        for (const std::string& path : paths) {
            CHECK(path.find("not_delivered") == std::string::npos);
        }
        CHECK(fs::is_directory(outDir / "partial_textures"));
    }

    // --- usdz still packages everything inside the archive ---
    {
        usdprep::ExtractOptions options;
        options.udimAtlas = false;  // the set-stays-a-set path; the atlas has its own test
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "panel.usdz").string();
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(fs::exists(options.outputPath));
        CHECK(!fs::exists(outDir / "panel_textures" / "panel.usdz"));
        std::string err;
        const usdprep::StageInfo info = usdprep::InspectStage(options.outputPath, &err);
        CHECK(err.empty());
        CHECK(info.counts.meshes == 1);
    }

    if (failures == 0) std::printf("test_relink: OK\n");
    return failures == 0 ? 0 : 1;
}
