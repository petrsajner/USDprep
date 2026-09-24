// OBJ in: objects, materials, UVs and normals as the file has them, what
// cannot come along reported - from a folder with Czech letters in its
// name, the kind a studio drive is full of. Then a far-away scan, reduced
// to a hundredth on the way to Nuke.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <usdprep/Extract.h>
#include <usdprep/Import.h>

PXR_NAMESPACE_USING_DIRECTIVE

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
        if (e.detail.find(text) != std::string::npos) return true;
    }
    return false;
}

float InputFloat(const UsdStageRefPtr& stage, const char* shader, const char* input) {
    float value = -1.0f;
    UsdShadeShader(stage->GetPrimAtPath(SdfPath(shader))).GetInput(TfToken(input)).Get(&value);
    return value;
}

size_t FaceCount(const UsdStageRefPtr& stage) {
    size_t faces = 0;
    for (const UsdPrim& prim : stage->Traverse()) {
        VtIntArray counts;
        if (prim.IsA<UsdGeomMesh>() && UsdGeomMesh(prim).GetFaceVertexCountsAttr().Get(&counts)) faces += counts.size();
    }
    return faces;
}

}  // namespace

int main() {
    // "Předávka" - spelled in bytes so the source file's encoding cannot matter
    const fs::path outDir = fs::current_path() / "import_out" / fs::u8path("P\xC5\x99" "ed\xC3\xA1vka");
    fs::remove_all(outDir.parent_path());
    fs::create_directories(outDir);
    for (const char* name : {"two_objects.obj", "two_objects.mtl"}) {
        fs::copy_file(fs::path(FIXTURE_DIR) / "obj" / name, outDir / name);
    }
    // the .mtl names C:\somewhere\else\wood.png; the texture travelled next to it
    fs::copy_file(fs::path(FIXTURE_DIR) / "textures" / "grey128.png", outDir / "wood.png");

    CHECK(usdprep::IsImportable("scan.OBJ"));
    CHECK(!usdprep::IsImportable("scene.usd"));

    // --- objects, materials, what is left out -----------------------------------
    const std::string objPath = (outDir / "two_objects.obj").u8string();
    const std::string usdPath = (outDir / "two_objects.usdc").u8string();
    usdprep::ImportProgress progress;
    const usdprep::Report rep = usdprep::ImportToUsd(objPath, usdPath, &progress);
    std::printf("%s\n", rep.ToText().c_str());
    CHECK(rep.ok);
    CHECK(progress.fraction == 1.0f);
    CHECK(Reported(rep, "2 object(s), 4 polygons"));
    CHECK(Reported(rep, "1 line(s)"));
    CHECK(Reported(rep, "fewer than 3 corners"));
    CHECK(Reported(rep, "found next to the .mtl"));
    CHECK(Reported(rep, "not found: panel_normal.png"));
    CHECK(Reported(rep, "material subsets"));

    UsdStageRefPtr stage = UsdStage::Open(usdPath);
    CHECK(stage);
    if (stage) {
        CHECK(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->y);
        CHECK(stage->GetDefaultPrim().GetPath() == SdfPath("/two_objects"));
        // looked up as one key: GetValueAtPath would split it at the colon
        const VtDictionary layerData = stage->GetRootLayer()->GetCustomLayerData();
        const auto found = layerData.find(usdprep::kImportedFromKey);
        const VtValue from = found != layerData.end() ? found->second : VtValue();
        CHECK(from.IsHolding<std::string>() && fs::u8path(from.Get<std::string>()) == fs::path(outDir / "two_objects.obj"));

        UsdGeomMesh crate(stage->GetPrimAtPath(SdfPath("/two_objects/Crate")));
        CHECK(crate);
        VtIntArray counts;
        crate.GetFaceVertexCountsAttr().Get(&counts);
        CHECK(counts.size() == 1 && counts[0] == 4);  // the quad stays a quad
        TfToken scheme;
        crate.GetSubdivisionSchemeAttr().Get(&scheme);
        CHECK(scheme == UsdGeomTokens->none);
        const UsdGeomPrimvar crateSt = UsdGeomPrimvarsAPI(crate).GetPrimvar(TfToken("st"));
        CHECK(crateSt && crateSt.GetInterpolation() == UsdGeomTokens->vertex);
        CHECK(UsdShadeMaterialBindingAPI(crate.GetPrim()).ComputeBoundMaterial().GetPath() ==
              SdfPath("/two_objects/Materials/Wood"));

        // "Sign Post": three triangles over two materials, a UV seam at the second one
        UsdGeomMesh post(stage->GetPrimAtPath(SdfPath("/two_objects/Sign_Post")));
        CHECK(post);
        post.GetFaceVertexCountsAttr().Get(&counts);
        CHECK(counts.size() == 3);
        VtIntArray indices;
        post.GetFaceVertexIndicesAttr().Get(&indices);
        CHECK(indices.size() == 9 && indices[0] == 0 && indices[1] == 1 && indices[2] == 2);  // f -5 -4 -3
        const UsdGeomPrimvar postSt = UsdGeomPrimvarsAPI(post).GetPrimvar(TfToken("st"));
        CHECK(postSt && postSt.GetInterpolation() == UsdGeomTokens->faceVarying && postSt.IsIndexed());
        const std::vector<UsdGeomSubset> subsets = UsdGeomSubset::GetAllGeomSubsets(post);
        CHECK(subsets.size() == 2);
        TfToken normalsInterpolation = post.GetNormalsInterpolation();
        CHECK(normalsInterpolation == UsdGeomTokens->vertex);

        // the materials
        CHECK(std::fabs(InputFloat(stage, "/two_objects/Materials/Wood/PreviewSurface", "roughness") - 0.5f) < 1e-4f);
        CHECK(std::fabs(InputFloat(stage, "/two_objects/Materials/Paint/PreviewSurface", "opacity") - 0.5f) < 1e-4f);
        CHECK(std::fabs(InputFloat(stage, "/two_objects/Materials/Metal/PreviewSurface", "metallic") - 1.0f) < 1e-4f);
        CHECK(stage->GetPrimAtPath(SdfPath("/two_objects/Materials/Metal/normalTexture")));  // -bm: a normal map
        SdfAssetPath wood;
        UsdShadeShader(stage->GetPrimAtPath(SdfPath("/two_objects/Materials/Wood/diffuseTexture")))
            .GetInput(TfToken("file"))
            .Get(&wood);
        CHECK(fs::u8path(wood.GetAssetPath()) == fs::path(outDir / "wood.png"));
    }

    // --- on to Nuke: the per-face materials come out as separate meshes -----------
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/two_objects"};
        options.outputPath = (outDir / "for_nuke.usdc").u8string();
        const usdprep::Report exported = usdprep::ExtractPrims(usdPath, options);
        CHECK(exported.ok);
        const UsdStageRefPtr out = UsdStage::Open(options.outputPath);
        CHECK(out && FaceCount(out) == 4);
        CHECK(out && out->GetRootLayer()->GetCustomLayerData().count(usdprep::kImportedFromKey) == 0);
        CHECK(fs::exists(outDir / "for_nuke_textures" / "0" / "wood.png"));  // the texture travels with it
    }

    // --- one object alone: its material lives beside it, and still comes along ------
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/two_objects/Crate"};
        options.outputPath = (outDir / "crate_only.usdc").u8string();
        const usdprep::Report exported = usdprep::ExtractPrims(usdPath, options);
        std::printf("%s\n", exported.ToText().c_str());
        CHECK(exported.ok);
        CHECK(Reported(exported, "came along from elsewhere"));
        const UsdStageRefPtr out = UsdStage::Open(options.outputPath);
        CHECK(out && out->GetPrimAtPath(SdfPath("/two_objects/Materials/Wood")));
        CHECK(out && UsdShadeMaterialBindingAPI(out->GetPrimAtPath(SdfPath("/two_objects/Crate")))
                             .ComputeBoundMaterial()
                             .GetPath() == SdfPath("/two_objects/Materials/Wood"));
        CHECK(fs::exists(outDir / "crate_only_textures" / "0" / "wood.png"));
        CHECK(out && !out->GetPrimAtPath(SdfPath("/two_objects/Sign_Post")));
    }

    // --- a scan at survey coordinates, with colours, down to a hundredth -----------
    {
        const fs::path scan = outDir / "scan_far.obj";
        {
            std::ofstream out(scan, std::ios::binary);
            const int n = 120;  // 120 x 120 quads, a gently waved sheet
            for (int j = 0; j <= n; ++j) {
                for (int i = 0; i <= n; ++i) {
                    const double x = 4500000.0 + i * 0.05, z = 5600000.0 + j * 0.05;
                    const double y = 312.0 + 0.1 * std::sin(i * 0.3) * std::cos(j * 0.2);
                    char line[160];
                    std::snprintf(line, sizeof(line), "v %.4f %.4f %.4f %d %d %d\n", x, y, z, i * 2, j * 2, 128);
                    out << line;
                }
            }
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const int a = j * (n + 1) + i + 1;
                    out << "f " << a << " " << a + 1 << " " << a + n + 2 << " " << a + n + 1 << "\n";
                }
            }
        }
        const std::string scanUsd = (outDir / "scan_far.usdc").u8string();
        const usdprep::Report scanRep = usdprep::ImportToUsd(scan.u8string(), scanUsd);
        std::printf("%s\n", scanRep.ToText().c_str());
        CHECK(scanRep.ok);
        CHECK(Reported(scanRep, "far from the origin"));
        CHECK(Reported(scanRep, "scaled to 0-1"));
        const UsdStageRefPtr stage2 = UsdStage::Open(scanUsd);
        CHECK(stage2);
        if (stage2) {
            // the root holds the offset in double precision, the points are small
            const UsdGeomXformable root(stage2->GetPrimAtPath(SdfPath("/scan_far")));
            bool resetsStack = false;
            const std::vector<UsdGeomXformOp> ops = root.GetOrderedXformOps(&resetsStack);
            CHECK(ops.size() == 1 && ops[0].GetPrecision() == UsdGeomXformOp::PrecisionDouble);
            GfVec3d translate(0.0);
            if (!ops.empty()) ops[0].Get(&translate);
            CHECK(std::fabs(translate[0] - 4500003.0) < 1e-6 && std::fabs(translate[2] - 5600003.0) < 1e-6);
            UsdGeomMesh mesh(stage2->GetPrimAtPath(SdfPath("/scan_far/scan_far")));
            VtVec3fArray points;
            mesh.GetPointsAttr().Get(&points);
            CHECK(points.size() == 121 * 121);
            // 0.05 apart, exactly enough to tell apart: no float grid at millions
            if (points.size() > 2) CHECK(std::fabs((points[1][0] - points[0][0]) - 0.05f) < 1e-5f);
            VtVec3fArray colors;
            mesh.GetDisplayColorPrimvar().Get(&colors);
            CHECK(colors.size() == points.size() && colors.back()[0] <= 1.0f);
        }

        usdprep::ExtractOptions options;
        options.primPaths = {"/scan_far"};
        options.outputPath = (outDir / "scan_far_small.usdc").u8string();
        options.simplifyRatio = 0.01;
        const usdprep::Report reduced = usdprep::ExtractPrims(scanUsd, options);
        std::printf("%s\n", reduced.ToText().c_str());
        CHECK(reduced.ok);
        const UsdStageRefPtr reducedStage = UsdStage::Open(options.outputPath);
        // 28,800 triangles asked down to a hundredth
        CHECK(reducedStage && FaceCount(reducedStage) <= 600);

        // the mesh alone, not its root: the file shrinks with it (a .usdc saved over itself
        // would keep the full arrays it held)
        usdprep::ExtractOptions alone = options;
        alone.primPaths = {"/scan_far/scan_far"};
        alone.outputPath = (outDir / "scan_mesh_small.usdc").u8string();
        const usdprep::Report meshOnly = usdprep::ExtractPrims(scanUsd, alone);
        CHECK(meshOnly.ok);
        std::error_code sizeError;
        const auto full = fs::file_size(fs::u8path(scanUsd), sizeError);
        const auto reducedSize = fs::file_size(fs::u8path(alone.outputPath), sizeError);
        std::printf("scan %llu bytes, a hundredth of its mesh %llu bytes\n", (unsigned long long)full,
                    (unsigned long long)reducedSize);
        CHECK(reducedSize * 10 < full);
    }

    // --- nothing to read -----------------------------------------------------------
    {
        const usdprep::Report missing = usdprep::ImportToUsd((outDir / "nope.obj").u8string(),
                                                             (outDir / "nope.usdc").u8string());
        CHECK(!missing.ok);
    }

    if (failures == 0) std::printf("all import tests passed\n");
    return failures == 0 ? 0 : 1;
}
