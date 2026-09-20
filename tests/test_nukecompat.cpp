// What is left after the recipe, made readable for Nuke: converted or
// replaced, never silently dropped, and every substitution in the report.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>

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

bool Reported(const usdprep::Report& rep, const std::string& text) {
    for (const usdprep::ReportEntry& e : rep.entries) {
        if (e.action == "nuke" && e.detail.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/nuke_compat_scene.usda";
    const fs::path outDir = fs::current_path() / "nukecompat_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- everything kept on purpose: lights on, proxies kept, renderer
    //     outputs kept - and still nothing Nuke chokes on ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "kept.usdc").string();
        options.stripRenderContexts = false;
        options.stripUnusedMaterials = false;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(stage);

        // the proxy is still there, hidden for good (its animation would unhide it)
        const pxr::UsdPrim proxy = stage->GetPrimAtPath(pxr::SdfPath("/Root/Proxy"));
        CHECK(proxy);
        pxr::TfToken visibility;
        const pxr::UsdAttribute visAttr = pxr::UsdGeomImageable(proxy).GetVisibilityAttr();
        CHECK(visAttr.Get(&visibility, 1.0) && visibility == pxr::UsdGeomTokens->invisible);
        CHECK(visAttr.GetNumTimeSamples() == 0);
        CHECK(Reported(rep, "guide/proxy object(s) kept but hidden"));

        // MaterialX output gone, the standard surface and the shader stay
        const pxr::UsdPrim both = stage->GetPrimAtPath(pxr::SdfPath("/Root/Looks/Both"));
        CHECK(!both.GetAttribute(pxr::TfToken("outputs:mtlx:surface")));
        CHECK(both.GetAttribute(pxr::TfToken("outputs:surface")));
        CHECK(Reported(rep, "MaterialX output(s) removed"));

        // the mesh Nuke would not draw is unbound; the material itself stays
        const pxr::UsdPrim exotic = stage->GetPrimAtPath(pxr::SdfPath("/Root/Exotic"));
        CHECK(!exotic.GetRelationship(pxr::TfToken("material:binding")));
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Looks/RendererOnly")));
        CHECK(Reported(rep, "/Root/Looks/RendererOnly"));
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Render")).GetRelationship(pxr::TfToken("material:binding")));

        // the light Nuke reads stays a light; the others become axes that
        // keep their name, their place and their settings
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Bulb")).GetTypeName() == "SphereLight");
        const pxr::UsdPrim sun = stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Sun"));
        CHECK(sun.GetTypeName() == "Xform");
        CHECK(sun.GetAttribute(pxr::TfToken("inputs:intensity")).HasAuthoredValue());
        CHECK(sun.GetAttribute(pxr::TfToken("xformOp:rotateXYZ")).HasAuthoredValue());
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Panel")).GetTypeName() == "Xform");
        CHECK(Reported(rep, "light /Root/Lights/Sun (DistantLight) replaced by an axis"));
        CHECK(Reported(rep, "light /Root/Lights/Panel (RectLight) replaced by an axis"));
        CHECK(Reported(rep, "1 light(s) kept"));
        CHECK(rep.after.lights == 1);
    }

    // --- the nuke preset: lights and proxies are simply not there ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        usdprep::ExtractOptions options;
        usdprep::ApplyRecipe(nuke, &options);
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "preset.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(!stage->GetPrimAtPath(pxr::SdfPath("/Root/Proxy")));
        CHECK(!stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Bulb")));
        CHECK(!stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Sun")));
        CHECK(rep.after.lights == 0);
        CHECK(!Reported(rep, "replaced by an axis"));
    }

    // --- as it is: nothing converted ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "asis.usdc").string();
        options.stripRenderContexts = false;
        options.stripUnusedMaterials = false;
        options.nukeCompat = false;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Lights/Sun")).GetTypeName() == "DistantLight");
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Looks/Both")).GetAttribute(pxr::TfToken("outputs:mtlx:surface")));
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root/Exotic")).GetRelationship(pxr::TfToken("material:binding")));
    }

    // --- skinning becomes a point cache: Nuke shows the bind pose otherwise ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "skel.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/skel_scene.usda", options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        const pxr::UsdAttribute points =
            stage->GetPrimAtPath(pxr::SdfPath("/Root/Quad")).GetAttribute(pxr::TfToken("points"));
        pxr::VtArray<pxr::GfVec3f> first, last;
        CHECK(points.GetNumTimeSamples() >= 2);
        CHECK(points.Get(&first, 1.0) && points.Get(&last, 10.0));
        CHECK(!first.empty() && first[0][0] < -1.0f);  // carried left by the joint at frame 1
        CHECK(!last.empty() && last[0][0] > 0.0f);     // and right at frame 10
        // no longer a SkelRoot: nothing skins the baked points a second time
        CHECK(stage->GetPrimAtPath(pxr::SdfPath("/Root")).GetTypeName() == "Xform");
        CHECK(Reported(rep, "skeletal animation baked"));
    }

    // --- a Z-up scene is stood up ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "zup.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/zup_scene.usda", options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(pxr::UsdGeomGetStageUpAxis(stage) == pxr::UsdGeomTokens->y);
        pxr::UsdGeomBBoxCache cache(pxr::UsdTimeCode::Default(), {pxr::UsdGeomTokens->default_});
        const pxr::GfRange3d box = cache.ComputeWorldBound(stage->GetPrimAtPath(pxr::SdfPath("/Root"))).ComputeAlignedRange();
        CHECK(box.GetSize()[1] > 1.9 && box.GetSize()[2] < 0.7);  // tall along Y now
        CHECK(Reported(rep, "the scene was Z-up"));

        options.outputPath = (outDir / "zup_asis.usdc").string();
        options.nukeCompat = false;
        CHECK(usdprep::ExtractPrims(FIXTURE_DIR "/zup_scene.usda", options).ok);
        CHECK(pxr::UsdGeomGetStageUpAxis(pxr::UsdStage::Open(options.outputPath)) == pxr::UsdGeomTokens->z);
    }

    // --- the presets ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.nukeCompat);
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(!raw.nukeCompat);
        CHECK(raw.dropTypes.empty());
    }

    if (failures == 0) std::printf("test_nukecompat: OK\n");
    return failures == 0 ? 0 : 1;
}
