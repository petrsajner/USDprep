// Alembic and FBX in, as USD: the scene each test writes or carries is
// known exactly, so is what has to come out.
//   .abc - written here with the Alembic library: an animated group with a
//          mesh (UVs, colours, a face set), a camera with a lens squeeze,
//          points, curves and a subdivision surface, on frames 1001-1003
//   .fbx - tests/data/fbx/rig.fbx, written by hand: a moving box, a bar
//          skinned to two bones, a camera, on frames 1001-1003
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/base/gf/camera.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <usdprep/Extract.h>
#include <usdprep/Import.h>

#ifdef TEST_WITH_ALEMBIC
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace fs = std::filesystem;

namespace {

bool Near(const GfVec3d& a, const GfVec3d& b, double eps = 1e-4) { return (a - b).GetLength() < eps; }
bool Near(const GfVec3f& a, const GfVec3f& b, float eps = 1e-4f) { return (a - b).GetLength() < eps; }

bool Reported(const usdprep::Report& rep, const std::string& text) {
    for (const usdprep::ReportEntry& e : rep.entries) {
        if (e.detail.find(text) != std::string::npos) return true;
    }
    return false;
}

#ifdef TEST_WITH_ALEMBIC
void WriteAlembic(const std::string& path) {
    using namespace Alembic::AbcGeom;
    OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
    const uint32_t frames = archive.addTimeSampling(TimeSampling(1.0 / 24.0, 1001.0 / 24.0));
    OObject top(archive, kTop);

    // a group moving along X, one unit a frame
    OXform group(top, "group", frames);
    for (int f = 0; f < 3; ++f) {
        XformSample move;
        move.setTranslation(V3d(f, 0, 0));
        group.getSchema().set(move);
    }

    // a box: a transform with one mesh below it - one prim in USD
    OXform boxTransform(group, "box");
    XformSample still;
    boxTransform.getSchema().set(still);
    OPolyMesh box(boxTransform, "boxShape");
    const std::vector<V3f> points{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}};
    // Alembic winds clockwise: the quad 0-1-2-3 and the triangle 1-4-2 the other way round
    const std::vector<int32_t> indices{0, 3, 2, 1, 2, 4, 1};
    const std::vector<int32_t> counts{4, 3};
    const std::vector<V2f> uvValues{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {2, 0}};
    const std::vector<uint32_t> uvIndices{0, 3, 2, 1, 2, 4, 1};
    OV2fGeomParam::Sample uvs(V2fArraySample(uvValues), UInt32ArraySample(uvIndices), kFacevaryingScope);
    box.getSchema().set(OPolyMeshSchema::Sample(P3fArraySample(points), Int32ArraySample(indices),
                                                Int32ArraySample(counts), uvs));
    OC3fGeomParam colours(box.getSchema().getArbGeomParams(), "Cd", false, kVertexScope, 1);
    const std::vector<C3f> colourValues{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 1, 1}};
    colours.set(OC3fGeomParam::Sample(C3fArraySample(colourValues), kVertexScope));
    OFaceSet top_ = box.getSchema().createFaceSet("top");
    const std::vector<int32_t> topFaces{0};
    top_.getSchema().set(OFaceSetSchema::Sample(Int32ArraySample(topFaces)));

    // a camera: 35 mm, a 36 x 24 mm back (Alembic: centimetres), a 2x anamorphic squeeze
    OXform cameraTransform(top, "cam");
    cameraTransform.getSchema().set(still);
    OCamera camera(cameraTransform, "camShape");
    CameraSample lens;
    lens.setFocalLength(35.0);
    lens.setHorizontalAperture(3.6);
    lens.setVerticalAperture(2.4);
    lens.setLensSqueezeRatio(2.0);
    camera.getSchema().set(lens);

    // points, curves, a subdivision surface
    OPoints cloud(group, "pts");
    const std::vector<V3f> cloudPoints{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}};
    const std::vector<uint64_t> ids{7, 8, 9};
    const std::vector<float> widths{0.1f, 0.2f, 0.3f};
    cloud.getSchema().set(OPointsSchema::Sample(P3fArraySample(cloudPoints), UInt64ArraySample(ids), V3fArraySample(),
                                                OFloatGeomParam::Sample(FloatArraySample(widths), kVertexScope)));
    OCurves hair(group, "hair");
    const std::vector<V3f> hairPoints{{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}};
    const std::vector<int32_t> hairCounts{2, 2};
    hair.getSchema().set(OCurvesSchema::Sample(P3fArraySample(hairPoints), Int32ArraySample(hairCounts), kLinear));
    OSubD smooth(group, "smooth");
    const std::vector<int32_t> quad{0, 3, 2, 1};
    const std::vector<int32_t> quadCount{4};
    OSubDSchema::Sample surface(P3fArraySample(std::vector<V3f>(points.begin(), points.begin() + 4)),
                                Int32ArraySample(quad), Int32ArraySample(quadCount));
    const std::vector<int32_t> creaseIndices{0, 1};
    const std::vector<int32_t> creaseLengths{2};
    const std::vector<float> creaseSharpness{2.0f};
    surface.setCreaseIndices(Int32ArraySample(creaseIndices));
    surface.setCreaseLengths(Int32ArraySample(creaseLengths));
    surface.setCreaseSharpnesses(FloatArraySample(creaseSharpness));
    surface.setSubdivisionScheme("catmull-clark");
    smooth.getSchema().set(surface);
}
#endif

}  // namespace

int main() {
    const fs::path outDir = fs::current_path() / "import_formats_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    CHECK(usdprep::IsImportable("shot.ABC"));
    CHECK(usdprep::IsImportable("character.fbx"));

#ifdef TEST_WITH_ALEMBIC
    // --- Alembic ----------------------------------------------------------------
    {
        const std::string abc = (outDir / "formats.abc").string();
        WriteAlembic(abc);
        const std::string usd = (outDir / "formats_abc.usdc").string();
        const usdprep::Report rep = usdprep::ImportToUsd(abc, usd);
        std::printf("%s\n", rep.ToText().c_str());
        CHECK(rep.ok);
        CHECK(Reported(rep, "frames 1001-1003 at 24 fps"));
        CHECK(Reported(rep, "Nuke's USD reader draws neither"));
        const UsdStageRefPtr stage = UsdStage::Open(usd);
        CHECK(stage);
        if (stage) {
            CHECK(stage->GetTimeCodesPerSecond() == 24.0);
            CHECK(stage->GetStartTimeCode() == 1001.0 && stage->GetEndTimeCode() == 1003.0);
            const UsdGeomXformable group(stage->GetPrimAtPath(SdfPath("/formats/group")));
            CHECK(group);
            CHECK(Near(group.ComputeLocalToWorldTransform(UsdTimeCode(1003.0)).ExtractTranslation(), GfVec3d(2, 0, 0)));

            // the box: transform and mesh in one prim, the faces turned to USD's winding
            const UsdGeomMesh box(stage->GetPrimAtPath(SdfPath("/formats/group/box")));
            CHECK(box);
            VtIntArray indices;
            box.GetFaceVertexIndicesAttr().Get(&indices);
            CHECK(indices == VtIntArray({1, 2, 3, 0, 1, 4, 2}));
            const UsdGeomPrimvar st = UsdGeomPrimvarsAPI(box).GetPrimvar(TfToken("st"));
            VtIntArray stIndices;
            CHECK(st && st.GetInterpolation() == UsdGeomTokens->faceVarying && st.GetIndices(&stIndices));
            CHECK(stIndices == VtIntArray({1, 2, 3, 0, 1, 4, 2}));
            VtVec3fArray colours;
            CHECK(box.GetDisplayColorPrimvar().Get(&colours) && colours.size() == 5 && Near(colours[0], GfVec3f(1, 0, 0)));
            CHECK(UsdGeomSubset::GetAllGeomSubsets(box).size() == 1);

            // the camera in millimetres, the squeeze in the horizontal back
            const UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath("/formats/cam")));
            CHECK(camera);
            const GfCamera lens = camera.GetCamera(UsdTimeCode::Default());
            CHECK(std::fabs(lens.GetFocalLength() - 35.0f) < 1e-3f);
            CHECK(std::fabs(lens.GetHorizontalAperture() - 72.0f) < 1e-3f);
            CHECK(std::fabs(lens.GetVerticalAperture() - 24.0f) < 1e-3f);

            const UsdGeomPoints cloud(stage->GetPrimAtPath(SdfPath("/formats/group/pts")));
            VtInt64Array ids;
            CHECK(cloud && cloud.GetIdsAttr().Get(&ids) && ids.size() == 3 && ids[0] == 7);
            const UsdGeomBasisCurves hair(stage->GetPrimAtPath(SdfPath("/formats/group/hair")));
            TfToken type;
            CHECK(hair && hair.GetTypeAttr().Get(&type) && type == UsdGeomTokens->linear);
            const UsdGeomMesh smooth(stage->GetPrimAtPath(SdfPath("/formats/group/smooth")));
            TfToken scheme;
            VtFloatArray sharpness;
            CHECK(smooth && smooth.GetSubdivisionSchemeAttr().Get(&scheme) && scheme == UsdGeomTokens->catmullClark);
            CHECK(smooth.GetCreaseSharpnessesAttr().Get(&sharpness) && sharpness.size() == 1);
        }
    }
#endif

    // --- FBX --------------------------------------------------------------------
    {
        const std::string fbx = FIXTURE_DIR "/fbx/rig.fbx";
        const std::string usd = (outDir / "rig_fbx.usdc").string();
        const usdprep::Report rep = usdprep::ImportToUsd(fbx, usd);
        std::printf("%s\n", rep.ToText().c_str());
        CHECK(rep.ok);
        CHECK(Reported(rep, "frames 1001-1003 at 24 fps"));  // assimp starts at 0; the file says 1001
        CHECK(Reported(rep, "skinned mesh"));
        const UsdStageRefPtr stage = UsdStage::Open(usd);
        CHECK(stage);
        if (stage) {
            const UsdGeomXformable box(stage->GetPrimAtPath(SdfPath("/rig/Box")));
            CHECK(box && Near(box.ComputeLocalToWorldTransform(UsdTimeCode(1002.0)).ExtractTranslation(), GfVec3d(1, 0, 0)));
            CHECK(box.GetPrim().GetRelationship(TfToken("material:binding")));

            // the bar: welded back to its six points, the far end turned with Bone2
            const UsdGeomMesh bar(stage->GetPrimAtPath(SdfPath("/rig/Bar")));
            VtVec3fArray start, end;
            CHECK(bar && bar.GetPointsAttr().Get(&start, UsdTimeCode(1001.0)) && start.size() == 6);
            CHECK(bar.GetPointsAttr().Get(&end, UsdTimeCode(1003.0)));
            bool farEndTurned = false;
            for (size_t i = 0; i < start.size() && i < end.size(); ++i) {
                if (Near(start[i], GfVec3f(2, 0, 0))) farEndTurned = Near(end[i], GfVec3f(1, 1, 0), 1e-3f);
            }
            CHECK(farEndTurned);
            CHECK(!bar.GetPrim().GetRelationship(TfToken("material:binding")));  // assimp's made-up grey stays out

            // the camera: the view exact, looking down -Z from (0, 0, 10)
            const UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath("/rig/Cam")));
            CHECK(camera);
            const GfCamera lens = camera.GetCamera(UsdTimeCode(1001.0));
            CHECK(std::fabs(lens.GetFieldOfView(GfCamera::FOVHorizontal) - 54.4322f) < 0.01f);
            const GfMatrix4d world = camera.ComputeLocalToWorldTransform(UsdTimeCode(1001.0));
            CHECK(Near(world.ExtractTranslation(), GfVec3d(0, 0, 10)));
            CHECK(Near(world.TransformDir(GfVec3d(0, 0, -1)), GfVec3d(0, 0, -1)));
        }

        // and on to Nuke: the skinned bar is a point cache already, it exports as one
        usdprep::ExtractOptions options;
        options.primPaths = {"/rig/Bar"};
        options.outputPath = (outDir / "bar_for_nuke.usdc").string();
        CHECK(usdprep::ExtractPrims(usd, options).ok);
    }

    // --- what cannot be read says so ------------------------------------------
    {
        const fs::path broken = outDir / "broken.fbx";
        { FILE* f = std::fopen(broken.string().c_str(), "wb"); std::fputs("not an fbx", f); std::fclose(f); }
        const usdprep::Report rep = usdprep::ImportToUsd(broken.string(), (outDir / "broken.usdc").string());
        CHECK(!rep.ok && !rep.error.empty());
        const fs::path brokenAbc = outDir / "broken.abc";
        { FILE* f = std::fopen(brokenAbc.string().c_str(), "wb"); std::fputs("not an abc", f); std::fclose(f); }
        const usdprep::Report abcRep = usdprep::ImportToUsd(brokenAbc.string(), (outDir / "broken_abc.usdc").string());
        CHECK(!abcRep.ok && !abcRep.error.empty());
    }

    if (failures == 0) std::printf("all import format tests passed\n");
    return failures == 0 ? 0 : 1;
}
