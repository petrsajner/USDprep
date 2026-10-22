// Repeating textures for Nuke (RepeatTiles.cpp), and a missing texture
// taken off its material: tests/data/repeat_scene.usda says what each
// object stands for.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <usdprep/Extract.h>

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

bool Reported(const usdprep::Report& rep, const std::string& text) {
    for (const usdprep::ReportEntry& e : rep.entries) {
        if (e.detail.find(text) != std::string::npos) return true;
    }
    return false;
}

struct Image {
    int width = 0, height = 0, channels = 0;
    std::vector<unsigned char> pixels;
    // x from the left, y from the top (image rows)
    GfVec3i At(int x, int y) const {
        const size_t at = (static_cast<size_t>(y) * width + x) * channels;
        return GfVec3i(pixels[at], pixels[at + 1], pixels[at + 2]);
    }
};

Image Load(const std::string& path) {
    Image out;
    const HioImageSharedPtr image = HioImage::OpenForReading(path, 0, 0, HioImage::SourceColorSpace::Raw);
    if (!image) return out;
    out.width = image->GetWidth();
    out.height = image->GetHeight();
    out.channels = HioGetComponentCount(image->GetFormat());
    out.pixels.resize(HioGetDataSize(image->GetFormat(), GfVec3i(out.width, out.height, 1)));
    HioImage::StorageSpec spec;
    spec.width = out.width;
    spec.height = out.height;
    spec.depth = 1;
    spec.format = image->GetFormat();
    spec.data = out.pixels.data();
    if (!image->Read(spec)) out.width = out.height = 0;
    return out;
}

const GfVec3i kRed(220, 30, 30), kGreen(30, 200, 30), kBlue(30, 60, 220), kYellow(230, 220, 30);

UsdShadeShader Source(const UsdShadeInput& input) {
    UsdShadeConnectableAPI source;
    TfToken name;
    UsdShadeAttributeType type;
    if (!input || !UsdShadeConnectableAPI::GetConnectedSource(input, &source, &name, &type)) return UsdShadeShader();
    return UsdShadeShader(source.GetPrim());
}

std::string FileOf(const UsdShadeShader& texture) {
    SdfAssetPath file;
    texture.GetInput(TfToken("file")).Get(&file);
    return file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
}

GfVec2f Vec(const UsdShadeShader& shader, const char* input) {
    GfVec2f value(-99.0f);
    if (const UsdShadeInput in = shader.GetInput(TfToken(input))) in.Get(&value);
    return value;
}

bool Near(const GfVec2f& a, const GfVec2f& b) { return (a - b).GetLength() < 1e-5f; }

}  // namespace

int main() {
    const fs::path outDir = fs::current_path() / "repeat_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    usdprep::ExtractOptions options;
    options.primPaths = {"/Root"};
    options.outputPath = (outDir / "repeat.usda").string();
    const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/repeat_scene.usda", options);
    std::printf("%s\n", rep.ToText().c_str());
    CHECK(rep.ok);
    CHECK(Reported(rep, "repeating texture(s) laid out over the tiles their UVs reach (up to 6 x 2)"));
    CHECK(Reported(rep, "1 texture(s) read by UVs that lie in another tile moved home"));
    CHECK(Reported(rep, "their materials show their own colour there"));
    for (const usdprep::ReportEntry& e : rep.entries) CHECK(e.action != "usd");  // no binding warnings

    const UsdStageRefPtr stage = UsdStage::Open(options.outputPath);
    CHECK(stage);
    if (!stage) return 1;
    const auto texture = [&](const char* material) {
        return UsdShadeShader(stage->GetPrimAtPath(SdfPath(std::string("/Root/Looks/") + material + "/Tex")));
    };

    // Tiled: 3 x 2 copies of the 4 x 4 texture, the UVs divided by (3, 2)
    {
        const UsdShadeShader tex = texture("Repeat");
        CHECK(FileOf(tex).find("quads.repeat3x2.png") != std::string::npos);
        const Image image = Load(FileOf(tex));
        CHECK(image.width == 12 && image.height == 8);
        if (image.width == 12 && image.height == 8) {
            CHECK(image.At(0, 7) == kRed && image.At(2, 7) == kGreen && image.At(4, 7) == kRed);  // bottom row
            CHECK(image.At(0, 0) == kBlue && image.At(11, 0) == kYellow && image.At(8, 6) == kRed && image.At(8, 4) == kBlue);
        }
        const UsdShadeShader squeeze = Source(tex.GetInput(TfToken("st")));
        CHECK(squeeze && squeeze.GetPath().GetName() == "Tex_tiles");
        CHECK(Near(Vec(squeeze, "scale"), GfVec2f(1.0f / 3.0f, 0.5f)) && Near(Vec(squeeze, "translation"), GfVec2f(0.0f)));
        CHECK(Source(squeeze.GetInput(TfToken("in"))).GetPath().GetName() == "Reader");
    }
    // Mirrored: tile -1 is mirrored in u, tile 0 is as it is
    {
        const UsdShadeShader tex = texture("Mirror");
        CHECK(FileOf(tex).find("quads.mirror2x1odd.png") != std::string::npos);
        const Image image = Load(FileOf(tex));
        CHECK(image.width == 8 && image.height == 4);
        if (image.width == 8 && image.height == 4) {
            CHECK(image.At(0, 3) == kGreen && image.At(3, 3) == kRed);  // mirrored
            CHECK(image.At(4, 3) == kRed && image.At(7, 3) == kGreen);  // as it is
        }
        const UsdShadeShader squeeze = Source(tex.GetInput(TfToken("st")));
        CHECK(Near(Vec(squeeze, "scale"), GfVec2f(0.5f, 1.0f)) && Near(Vec(squeeze, "translation"), GfVec2f(0.5f, 0.0f)));
    }
    // Scaled: the asset's own transform (u x 6) folded into one node
    {
        const UsdShadeShader tex = texture("Scaled");
        CHECK(FileOf(tex).find("quads.repeat6x1.png") != std::string::npos);
        const UsdShadeShader squeeze = Source(tex.GetInput(TfToken("st")));
        CHECK(squeeze && squeeze.GetPath().GetName() == "Tex_tiles");
        CHECK(Near(Vec(squeeze, "scale"), GfVec2f(1.0f, 1.0f)) && Near(Vec(squeeze, "translation"), GfVec2f(0.0f)));
        CHECK(Source(squeeze.GetInput(TfToken("in"))).GetPath().GetName() == "Reader");
    }
    // Clamped: as it was
    {
        const UsdShadeShader tex = texture("Clamp");
        CHECK(fs::path(FileOf(tex)).filename() == "quads.png");
        CHECK(Source(tex.GetInput(TfToken("st"))).GetPath().GetName() == "Reader");
    }
    // Shifted: the same texture, moved home by two tiles
    {
        const UsdShadeShader tex = texture("Shift");
        CHECK(fs::path(FileOf(tex)).filename() == "quads.png");
        const UsdShadeShader squeeze = Source(tex.GetInput(TfToken("st")));
        CHECK(Near(Vec(squeeze, "scale"), GfVec2f(1.0f)) && Near(Vec(squeeze, "translation"), GfVec2f(-2.0f, 0.0f)));
    }
    // Missing: off the surface, the colour underneath shows
    {
        const UsdShadeShader surface(stage->GetPrimAtPath(SdfPath("/Root/Looks/Gone/Surface")));
        const UsdShadeInput diffuse = surface.GetInput(TfToken("diffuseColor"));
        CHECK(diffuse && !diffuse.HasConnectedSource());
        GfVec3f colour(0.0f);
        CHECK(diffuse.Get(&colour) && (colour - GfVec3f(0.2f, 0.4f, 0.6f)).GetLength() < 1e-5f);
    }

    // without the Nuke fixes nothing is laid out
    {
        usdprep::ExtractOptions plain = options;
        plain.outputPath = (outDir / "plain.usda").string();
        plain.nukeCompat = false;
        const usdprep::Report plainRep = usdprep::ExtractPrims(FIXTURE_DIR "/repeat_scene.usda", plain);
        CHECK(plainRep.ok && !Reported(plainRep, "repeating texture"));
    }

    if (failures == 0) std::printf("all repeat tests passed\n");
    return failures == 0 ? 0 : 1;
}
