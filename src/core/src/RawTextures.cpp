// Nuke decodes every 8-bit texture as sRGB, whatever the file says
// (measured in 16.1 and 17.0: a roughness map of grey 128 marked
// sourceColorSpace = "raw" behaves like roughness 0.22, not 0.5 - and a
// colour texture marked raw comes out the same as one marked sRGB). For
// colour that is what the author meant almost always; for data - roughness,
// metallic, normal maps, opacity - it is wrong: surfaces turn glossier,
// normals bend.
//
// The fix is a copy of the texture with the sRGB curve applied once, so
// that Nuke's decoding lands on the numbers the author stored. Only 8-bit
// files that a UsdUVTexture reads as "raw" are touched; float textures are
// read linearly by Nuke and need nothing. The originals are never written.

#include "Shared.h"

#include <cmath>
#include <map>

#include <pxr/usd/usdShade/shader.h>

namespace usdprep {
namespace detail {

namespace {

namespace fs = std::filesystem;

// value -> the 8-bit code Nuke will decode back to `value`
unsigned char Encode(unsigned char stored) {
    const double v = stored / 255.0;
    const double encoded = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<unsigned char>(std::lround(std::min(std::max(encoded, 0.0), 1.0) * 255.0));
}

// Empty = not an 8-bit image this build can read and write; leave the texture alone.
std::string CompensatedCopy(const std::string& source, const fs::path& folder) {
    const HioImageSharedPtr image =
        HioImage::OpenForReading(source, 0, 0, HioImage::SourceColorSpace::Raw, /*suppressErrors=*/true);
    if (!image) return {};
    const HioFormat format = image->GetFormat();
    const HioType type = HioGetHioType(format);
    if (type != HioTypeUnsignedByte && type != HioTypeUnsignedByteSRGB) return {};
    const int width = image->GetWidth();
    const int height = image->GetHeight();
    const int channels = HioGetComponentCount(format);
    std::vector<unsigned char> pixels(HioGetDataSize(format, GfVec3i(width, height, 1)));
    HioImage::StorageSpec spec;
    spec.width = width;
    spec.height = height;
    spec.depth = 1;
    spec.format = format;
    spec.data = pixels.data();
    if (!image->Read(spec)) return {};

    unsigned char table[256];
    for (int i = 0; i < 256; ++i) table[i] = Encode(static_cast<unsigned char>(i));
    const int colour = channels == 4 ? 3 : channels == 2 ? 1 : channels;  // alpha is never encoded
    for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p) {
        for (int c = 0; c < colour; ++c) pixels[p * channels + c] = table[pixels[p * channels + c]];
    }

    char hashed[24];
    std::snprintf(hashed, sizeof(hashed), "%08zx",
                  std::hash<std::string>{}(fs::path(source).parent_path().string()) & 0xffffffffu);
    std::error_code ec;
    fs::create_directories(folder / hashed, ec);
    // always a PNG: a JPEG would put its own rounding on top of the curve
    const fs::path out = folder / hashed / (fs::path(source).stem().string() + ".nukedata.png");
    const HioImageSharedPtr writer = HioImage::OpenForWriting(out.string());
    if (!writer || !writer->Write(spec)) return {};
    return out.generic_string();
}

}  // namespace

void CompensateRawTextures(Report& rep, const UsdStageRefPtr& flat, const std::string& folder) {
    std::map<std::string, std::string> copies;  // source -> copy ("" = left alone)
    size_t shaders = 0;
    for (const UsdPrim& prim : flat->Traverse()) {
        const UsdShadeShader shader(prim);
        TfToken id;
        if (!shader || !shader.GetShaderId(&id) || id != "UsdUVTexture") continue;
        TfToken space;
        const UsdShadeInput spaceInput = shader.GetInput(TfToken("sourceColorSpace"));
        if (!spaceInput || !spaceInput.Get(&space) || space != "raw") continue;
        const UsdShadeInput file = shader.GetInput(TfToken("file"));
        SdfAssetPath asset;
        if (!file || !file.Get(&asset)) continue;
        const std::string source = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
        if (source.empty() || UsdShadeUdimUtils::IsUdimIdentifier(source)) continue;
        auto known = copies.find(source);
        if (known == copies.end()) known = copies.emplace(source, CompensatedCopy(source, fs::path(folder))).first;
        if (known->second.empty()) continue;
        file.Set(SdfAssetPath(known->second));
        ++shaders;
    }
    size_t made = 0;
    for (const auto& copy : copies) made += copy.second.empty() ? 0 : 1;
    if (made > 0) {
        rep.Info("nuke", std::to_string(made) + " 8-bit data texture(s) (roughness, normals, masks - marked raw) "
                             "re-encoded into copies for " + std::to_string(shaders) +
                             " shader(s): Nuke decodes every 8-bit texture as sRGB and would read the numbers wrong");
    }
}

}  // namespace detail
}  // namespace usdprep
