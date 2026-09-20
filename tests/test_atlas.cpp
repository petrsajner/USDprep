// The UDIM atlas: a tile set becomes one texture laid out like the UV
// space it covers, and the material squeezes the UVs into it.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/assetPath.h>
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

struct Pixels {
    int width = 0, height = 0, channels = 0;
    std::vector<unsigned char> data;
    // r, g, b packed as 0xRRGGBB; row 0 is the top of the image
    unsigned At(int x, int y) const {
        const unsigned char* p = &data[(static_cast<size_t>(y) * width + x) * channels];
        return (p[0] << 16) | (p[1] << 8) | p[2];
    }
};

Pixels ReadImage(const std::string& path) {
    Pixels px;
    const pxr::HioImageSharedPtr image = pxr::HioImage::OpenForReading(path);
    if (!image) return px;
    px.width = image->GetWidth();
    px.height = image->GetHeight();
    px.channels = pxr::HioGetComponentCount(image->GetFormat());
    px.data.resize(pxr::HioGetDataSize(image->GetFormat(), pxr::GfVec3i(px.width, px.height, 1)));
    pxr::HioImage::StorageSpec spec;
    spec.width = px.width;
    spec.height = px.height;
    spec.depth = 1;
    spec.format = image->GetFormat();
    spec.data = px.data.data();
    if (!image->Read(spec)) px = Pixels();
    return px;
}

std::string FileOf(const pxr::UsdStageRefPtr& stage, const char* shader) {
    pxr::SdfAssetPath asset;
    stage->GetPrimAtPath(pxr::SdfPath(shader)).GetAttribute(pxr::TfToken("inputs:file")).Get(&asset);
    return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
}

std::string SourceOf(const pxr::UsdStageRefPtr& stage, const char* attribute) {
    pxr::SdfPathVector sources;
    stage->GetAttributeAtPath(pxr::SdfPath(attribute)).GetConnections(&sources);
    return sources.empty() ? std::string() : sources.front().GetString();
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/udim_atlas_scene.usda";
    const fs::path outDir = fs::current_path() / "atlas_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- three tiles of a 2x2 grid that starts at column 1 ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "atlas.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(stage);

        // one atlas for the set, both shaders on it, next to the output
        const std::string file = FileOf(stage, "/Root/Looks/Wired/Tex");
        CHECK(fs::path(file).filename() == "grid.atlas.png");
        CHECK(file.find("atlas_textures") != std::string::npos);
        CHECK(FileOf(stage, "/Root/Looks/Bare/Tex") == file);

        // laid out like UV space: row 1 on top, row 0 below; column 1 left
        const Pixels px = ReadImage(file);
        CHECK(px.width == 16 && px.height == 16);
        if (px.width == 16 && px.height == 16) {
            CHECK(px.At(2, 2) == 0x0000ffu);    // 1012, blue
            CHECK(px.At(12, 2) == 0x000000u);   // 1013 was never painted
            CHECK(px.At(2, 12) == 0xff0000u);   // 1002, red
            CHECK(px.At(12, 12) == 0x00ff00u);  // 1003, green
        }

        // the transform sits between the texture and what fed its st
        CHECK(SourceOf(stage, "/Root/Looks/Wired/Tex.inputs:st") == "/Root/Looks/Wired/Tex_udimAtlas.outputs:result");
        CHECK(SourceOf(stage, "/Root/Looks/Wired/Tex_udimAtlas.inputs:in") == "/Root/Looks/Wired/Reader.outputs:result");
        pxr::GfVec2f scale(0.0f), translation(9.0f);
        stage->GetAttributeAtPath(pxr::SdfPath("/Root/Looks/Wired/Tex_udimAtlas.inputs:scale")).Get(&scale);
        stage->GetAttributeAtPath(pxr::SdfPath("/Root/Looks/Wired/Tex_udimAtlas.inputs:translation")).Get(&translation);
        CHECK(scale == pxr::GfVec2f(0.5f, 0.5f));
        CHECK(translation == pxr::GfVec2f(-0.5f, 0.0f));
        // a texture with no st connection gets a reader of its own
        CHECK(SourceOf(stage, "/Root/Looks/Bare/Tex_udimAtlas.inputs:in") ==
              "/Root/Looks/Bare/Tex_udimAtlasSt.outputs:result");

        bool reported = false;
        bool nukeWarning = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.detail.find("1 UDIM set(s) (3 tiles) stitched") != std::string::npos) reported = true;
            if (e.action == "nuke") nukeWarning = true;
        }
        CHECK(reported);
        CHECK(!nukeWarning);  // nothing left that Nuke cannot read
        // the tiles themselves are not shipped
        CHECK(!fs::exists(outDir / "atlas_textures" / "grid.1002.png"));
    }

    // --- the cap is per tile, not per atlas ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "capped.usdc").string();
        options.maxTextureSize = 4;
        CHECK(usdprep::ExtractPrims(scene, options).ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        const Pixels px = ReadImage(FileOf(stage, "/Root/Looks/Wired/Tex"));
        CHECK(px.width == 8 && px.height == 8);
    }

    // --- textures left where they are: the atlas still has to live somewhere ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "inplace.usdc").string();
        options.relinkTextures = false;
        CHECK(usdprep::ExtractPrims(scene, options).ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        const std::string file = FileOf(stage, "/Root/Looks/Wired/Tex");
        CHECK(fs::exists(file));
        CHECK(file.find("inplace_textures") != std::string::npos);
    }

    // --- switched off: the set stays a set, and the report says what Nuke will do ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "kept.usdc").string();
        options.udimAtlas = false;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(options.outputPath);
        CHECK(FileOf(stage, "/Root/Looks/Wired/Tex").find("<UDIM>") != std::string::npos);
        CHECK(!stage->GetPrimAtPath(pxr::SdfPath("/Root/Looks/Wired/Tex_udimAtlas")));
        bool nukeWarning = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "nuke") nukeWarning = true;
        }
        CHECK(nukeWarning);
    }

    // --- the presets ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.udimAtlas);
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(!raw.udimAtlas);
    }

    if (failures == 0) std::printf("test_atlas: OK\n");
    return failures == 0 ? 0 : 1;
}
