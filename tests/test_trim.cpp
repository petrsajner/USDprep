// Trim: the animation diet - shot range, single frame, or left alone.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Extract.h>
#include <usdprep/Recipe.h>

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

std::vector<double> Samples(const pxr::UsdStageRefPtr& stage, const char* attrPath) {
    std::vector<double> times;
    const pxr::UsdAttribute attr = stage->GetAttributeAtPath(pxr::SdfPath(attrPath));
    if (attr) attr.GetTimeSamples(&times);
    return times;
}

usdprep::Report Export(const std::string& scene, const fs::path& outPath, const std::string& animation,
                       double frameStart = usdprep::kStageFrame, double frameEnd = usdprep::kStageFrame,
                       double staticFrame = usdprep::kStageFrame) {
    usdprep::ExtractOptions options;
    options.primPaths = {"/Root"};
    options.outputPath = outPath.string();
    options.animation = animation;
    options.frameStart = frameStart;
    options.frameEnd = frameEnd;
    options.staticFrame = staticFrame;
    return usdprep::ExtractPrims(scene, options);
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/animated_scene.usda";
    const fs::path outDir = fs::current_path() / "trim_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- range: the shot's own frames stay, plus one bracketing sample
    //     on each side; the pre-roll goes ---
    {
        const usdprep::Report rep = Export(scene, outDir / "range.usdc", "range");
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "range.usdc").string());
        CHECK(stage);
        const std::vector<double> points = Samples(stage, "/Root/Cloth.points");
        CHECK(points == std::vector<double>({1000, 1001, 1002, 1005, 1010}));
        CHECK(Samples(stage, "/Root/Cloth.extent") == std::vector<double>({990, 1010}));  // both bracket
        CHECK(Samples(stage, "/Root/Cloth.xformOp:translate") == std::vector<double>({995, 1003}));
        CHECK(stage->GetStartTimeCode() == 1001);
        CHECK(stage->GetEndTimeCode() == 1005);
        bool reported = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "trim" && e.detail.find("1001-1005") != std::string::npos) reported = true;
        }
        CHECK(reported);
    }

    // --- range, explicit frames: the file's range follows ---
    {
        const usdprep::Report rep = Export(scene, outDir / "frames.usdc", "range", 1001, 1002);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "frames.usdc").string());
        CHECK(Samples(stage, "/Root/Cloth.points") == std::vector<double>({1000, 1001, 1002, 1005}));
        CHECK(stage->GetStartTimeCode() == 1001);
        CHECK(stage->GetEndTimeCode() == 1002);
    }

    // --- static: one frame becomes the value, interpolated where the
    //     frame falls between samples; nothing animates any more ---
    {
        const usdprep::Report rep = Export(scene, outDir / "static.usdc", "static",
                                          usdprep::kStageFrame, usdprep::kStageFrame, 1002);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "static.usdc").string());
        CHECK(Samples(stage, "/Root/Cloth.points").empty());
        CHECK(Samples(stage, "/Root/Cloth.xformOp:translate").empty());
        pxr::VtArray<pxr::GfVec3f> points;
        CHECK(stage->GetAttributeAtPath(pxr::SdfPath("/Root/Cloth.points")).Get(&points));
        CHECK(points.size() == 4 && std::fabs(points[0][1] - 1.2f) < 1e-5f);
        pxr::GfVec3d translate;
        CHECK(stage->GetAttributeAtPath(pxr::SdfPath("/Root/Cloth.xformOp:translate")).Get(&translate));
        CHECK(std::fabs(translate[2] - 2.625) < 1e-9);  // 995 -> 0, 1003 -> 3, so 1002 -> 2.625
        CHECK(stage->GetStartTimeCode() == 1002);
        CHECK(stage->GetEndTimeCode() == 1002);
        // the static mesh next door is untouched
        pxr::VtArray<pxr::GfVec3f> rock;
        CHECK(stage->GetAttributeAtPath(pxr::SdfPath("/Root/Rock.points")).Get(&rock));
        CHECK(rock.size() == 4);
    }

    // --- static without a frame: the range start ---
    {
        const usdprep::Report rep = Export(scene, outDir / "static_start.usdc", "static");
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "static_start.usdc").string());
        pxr::VtArray<pxr::GfVec3f> points;
        CHECK(stage->GetAttributeAtPath(pxr::SdfPath("/Root/Cloth.points")).Get(&points));
        CHECK(points.size() == 4 && std::fabs(points[0][1] - 1.1f) < 1e-5f);  // frame 1001
    }

    // --- all: untouched ---
    {
        const usdprep::Report rep = Export(scene, outDir / "all.usdc", "all");
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "all.usdc").string());
        CHECK(Samples(stage, "/Root/Cloth.points").size() == 7);
    }

    // --- the presets and the recipe file ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.animation == "range");
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(raw.animation == "all");

        const fs::path recipePath = outDir / "static.json";
        {
            std::FILE* f = std::fopen(recipePath.string().c_str(), "wb");
            CHECK(f != nullptr);
            if (f) {
                std::fputs("{\"preset\": \"nuke\", \"animation\": \"static\", \"staticFrame\": 1003}", f);
                std::fclose(f);
            }
        }
        usdprep::Recipe loaded;
        std::string error;
        CHECK(usdprep::LoadRecipe(recipePath.string(), &loaded, &error));
        CHECK(loaded.animation == "static");
        CHECK(loaded.staticFrame == 1003);
        CHECK(std::isnan(loaded.frameStart));  // still the stage's own
        CHECK(usdprep::RecipeToJson(loaded).find("\"staticFrame\": 1003") != std::string::npos);
    }

    if (failures == 0) std::printf("test_trim: OK\n");
    return failures == 0 ? 0 : 1;
}
