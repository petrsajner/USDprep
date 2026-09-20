// Simplify: fewer faces, the attributes still there, the small mesh
// untouched, and nothing at all unless asked for.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

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

usdprep::Report Export(const std::string& scene, const fs::path& outPath, double ratio) {
    usdprep::ExtractOptions options;
    options.primPaths = {"/Root"};
    options.outputPath = outPath.string();
    options.simplifyRatio = ratio;
    return usdprep::ExtractPrims(scene, options);
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/dense_scene.usda";
    const fs::path outDir = fs::current_path() / "simplify_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- a quarter of the triangles: fewer faces, all triangles, UVs and
    //     normals per vertex, the seam still a seam (split vertices) ---
    {
        const usdprep::Report rep = Export(scene, outDir / "quarter.usdc", 0.25);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "quarter.usdc").string());
        CHECK(stage);
        pxr::UsdGeomMesh terrain(stage->GetPrimAtPath(pxr::SdfPath("/Root/Terrain")));
        CHECK(terrain);
        pxr::VtArray<int> counts;
        pxr::VtArray<int> indices;
        pxr::VtArray<pxr::GfVec3f> points;
        terrain.GetFaceVertexCountsAttr().Get(&counts);
        terrain.GetFaceVertexIndicesAttr().Get(&indices);
        terrain.GetPointsAttr().Get(&points);
        // 1600 quads = 3200 triangles; a quarter is 800, and the ratio is
        // the contract (no error ceiling), so it lands close
        CHECK(counts.size() >= 700 && counts.size() <= 900);
        bool allTriangles = true;
        for (const int c : counts) allTriangles = allTriangles && c == 3;
        CHECK(allTriangles);
        CHECK(indices.size() == counts.size() * 3);
        CHECK(!points.empty() && points.size() < 1681);
        for (const int i : indices) CHECK(i >= 0 && static_cast<size_t>(i) < points.size());

        const pxr::UsdGeomPrimvar st = pxr::UsdGeomPrimvarsAPI(terrain.GetPrim()).GetPrimvar(pxr::TfToken("st"));
        CHECK(st);
        CHECK(st.GetInterpolation() == pxr::UsdGeomTokens->vertex);
        pxr::VtArray<pxr::GfVec2f> uvs;
        st.Get(&uvs);
        CHECK(uvs.size() == points.size());
        CHECK(!st.IsIndexed());
        CHECK(terrain.GetNormalsInterpolation() == pxr::UsdGeomTokens->vertex);
        pxr::VtArray<pxr::GfVec3f> normals;
        terrain.GetNormalsAttr().Get(&normals);
        CHECK(normals.size() == points.size());
        pxr::VtArray<pxr::GfVec3f> extent;
        CHECK(terrain.GetExtentAttr().Get(&extent) && extent.size() == 2);
        pxr::TfToken scheme;
        terrain.GetSubdivisionSchemeAttr().Get(&scheme);
        CHECK(scheme == pxr::UsdGeomTokens->none);
        // the constant displayColor rides along untouched
        CHECK(pxr::UsdGeomPrimvarsAPI(terrain.GetPrim()).GetPrimvar(pxr::TfToken("displayColor")));

        // the pebble is below any minimum: still one quad
        pxr::UsdGeomMesh pebble(stage->GetPrimAtPath(pxr::SdfPath("/Root/Pebble")));
        pxr::VtArray<int> pebbleCounts;
        pebble.GetFaceVertexCountsAttr().Get(&pebbleCounts);
        CHECK(pebbleCounts.size() == 1 && pebbleCounts[0] == 4);

        bool reported = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            // 1600 quads are 3200 triangles; the ratio is about those
            if (e.action == "simplify" && e.detail.find("1 mesh(es) reduced from 3200") != std::string::npos) {
                reported = true;
            }
        }
        CHECK(reported);
    }

    // --- off by default: nothing changes ---
    {
        const usdprep::Report rep = Export(scene, outDir / "asis.usdc", 0.0);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "asis.usdc").string());
        pxr::UsdGeomMesh terrain(stage->GetPrimAtPath(pxr::SdfPath("/Root/Terrain")));
        pxr::VtArray<int> counts;
        terrain.GetFaceVertexCountsAttr().Get(&counts);
        CHECK(counts.size() == 1600);
        const pxr::UsdGeomPrimvar st = pxr::UsdGeomPrimvarsAPI(terrain.GetPrim()).GetPrimvar(pxr::TfToken("st"));
        CHECK(st.GetInterpolation() == pxr::UsdGeomTokens->faceVarying);
        for (const usdprep::ReportEntry& e : rep.entries) CHECK(e.action != "simplify");
    }

    // --- no preset turns it on ---
    {
        for (const std::string& name : usdprep::PresetNames()) {
            usdprep::Recipe recipe;
            usdprep::GetPreset(name, &recipe);
            CHECK(recipe.simplifyRatio == 0.0);
        }
    }

    if (failures == 0) std::printf("test_simplify: OK\n");
    return failures == 0 ? 0 : 1;
}
