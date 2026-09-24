// UDIM atlas: Nuke does not expand a "<UDIM>" template, so a material on
// a multi-tile set renders black there. Nuke does honour UsdTransform2d
// (checked by rendering, tools/nuke/make_transform2d_probe.py), which
// makes the fix local to the material: the tiles are stitched into one
// image laid out like the UV space they cover, and a UsdTransform2d in
// front of the texture squeezes the mesh's UVs into it. The meshes and
// their UVs are not touched, so it does not matter who else shares them.
//
//   tile 1001 + 10*row + column  ->  column, row in UV space
//   st' = (st - (minColumn, minRow)) / (columns, rows)

#include "Shared.h"

#include <cstring>
#include <map>

#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usdShade/shader.h>

namespace usdprep {
namespace detail {

namespace {

namespace fs = std::filesystem;

// Wider than this and GPUs (Nuke's viewer among them) start refusing the texture.
constexpr int kAtlasLimit = 16384;

struct Atlas {
    std::string path;  // empty = could not be built
    int minColumn = 0, minRow = 0, columns = 1, rows = 1;
    int tileWidth = 0, tileHeight = 0;
    bool scaled = false;
};

// "big.<UDIM>.png" -> "big.atlas.png"
std::string AtlasNameFor(const std::string& udimPath) {
    std::string name = fs::path(udimPath).filename().string();
    const size_t at = name.find("<UDIM>");
    if (at != std::string::npos) name.replace(at, 6, "atlas");
    return name;
}

Atlas BuildAtlas(const std::string& udimPath, const SdfLayerHandle& layer, const fs::path& atlasDir,
                 int maxTileSize, std::string* problem) {
    Atlas atlas;
    const auto tiles = UsdShadeUdimUtils::ResolveUdimTilePaths(udimPath, layer);
    if (tiles.size() < 2) return atlas;

    struct Tile {
        std::string path;
        int column, row;
    };
    std::vector<Tile> placed;
    int maxColumn = 0, maxRow = 0;
    atlas.minColumn = atlas.minRow = 1 << 20;
    for (const auto& tile : tiles) {
        const int index = std::atoi(tile.second.c_str()) - 1001;
        if (index < 0) continue;
        const Tile t{tile.first, index % 10, index / 10};
        atlas.minColumn = std::min(atlas.minColumn, t.column);
        atlas.minRow = std::min(atlas.minRow, t.row);
        maxColumn = std::max(maxColumn, t.column);
        maxRow = std::max(maxRow, t.row);
        placed.push_back(t);
    }
    if (placed.size() < 2) return atlas;
    atlas.columns = maxColumn - atlas.minColumn + 1;
    atlas.rows = maxRow - atlas.minRow + 1;

    // The first tile decides the pixel format and the tile size; Hio
    // resamples the others on read if they differ in size.
    HioFormat format = HioFormatInvalid;
    size_t pixelBytes = 0;
    std::vector<unsigned char> canvas;
    std::vector<unsigned char> buffer;
    for (const Tile& tile : placed) {
        const HioImageSharedPtr image = HioImage::OpenForReading(
            tile.path, 0, 0, HioImage::SourceColorSpace::Raw, /*suppressErrors=*/true);
        if (!image) {
            *problem = "cannot read " + fs::path(tile.path).filename().string();
            return Atlas();
        }
        if (format == HioFormatInvalid) {
            format = image->GetFormat();
            const int width = image->GetWidth();
            const int height = image->GetHeight();
            double factor = 1.0;
            if (maxTileSize > 0) factor = std::min(factor, static_cast<double>(maxTileSize) / std::max(width, height));
            factor = std::min(factor, static_cast<double>(kAtlasLimit) /
                                          std::max(width * atlas.columns, height * atlas.rows));
            atlas.scaled = factor < 1.0;
            atlas.tileWidth = std::max(1, static_cast<int>(width * factor));
            atlas.tileHeight = std::max(1, static_cast<int>(height * factor));
            pixelBytes = HioGetDataSize(format, GfVec3i(1, 1, 1));
            canvas.assign(pixelBytes * atlas.tileWidth * atlas.columns * atlas.tileHeight * atlas.rows, 0);
            buffer.resize(pixelBytes * atlas.tileWidth * atlas.tileHeight);
        } else if (image->GetFormat() != format) {
            *problem = "the tiles of " + fs::path(udimPath).filename().string() + " differ in pixel format";
            return Atlas();
        }
        HioImage::StorageSpec in;
        in.width = atlas.tileWidth;
        in.height = atlas.tileHeight;
        in.depth = 1;
        in.format = format;
        in.data = buffer.data();
        if (!image->Read(in)) {
            *problem = "cannot read " + fs::path(tile.path).filename().string();
            return Atlas();
        }
        // Image rows run top to bottom, UV rows bottom to top.
        const size_t left = static_cast<size_t>(tile.column - atlas.minColumn) * atlas.tileWidth;
        const size_t top = static_cast<size_t>(atlas.rows - 1 - (tile.row - atlas.minRow)) * atlas.tileHeight;
        const size_t canvasWidth = static_cast<size_t>(atlas.tileWidth) * atlas.columns;
        for (int y = 0; y < atlas.tileHeight; ++y) {
            std::memcpy(canvas.data() + ((top + y) * canvasWidth + left) * pixelBytes,
                        buffer.data() + static_cast<size_t>(y) * atlas.tileWidth * pixelBytes,
                        static_cast<size_t>(atlas.tileWidth) * pixelBytes);
        }
    }

    // Two sets that share a name live in different source folders.
    char folder[24];
    std::snprintf(folder, sizeof(folder), "%08zx",
                  std::hash<std::string>{}(fs::path(udimPath).parent_path().string()) & 0xffffffffu);
    std::error_code ec;
    fs::create_directories(atlasDir / folder, ec);
    const fs::path out = atlasDir / folder / AtlasNameFor(udimPath);
    const HioImageSharedPtr writer = HioImage::OpenForWriting(out.string());
    HioImage::StorageSpec spec;
    spec.width = atlas.tileWidth * atlas.columns;
    spec.height = atlas.tileHeight * atlas.rows;
    spec.depth = 1;
    spec.format = format;
    spec.data = canvas.data();
    if (!writer || !writer->Write(spec)) {
        *problem = "cannot write " + out.string();
        return Atlas();
    }
    atlas.path = out.generic_string();
    return atlas;
}

}  // namespace

void AtlasUdimTextures(Report& rep, const UsdStageRefPtr& flat, const std::string& atlasDir,
                       int maxTileSize) {
    static const TfToken kFile("inputs:file");
    std::map<std::string, Atlas> atlases;  // by UDIM template
    std::vector<std::string> problems;
    size_t rewired = 0, built = 0, tilesIn = 0, scaled = 0;

    std::vector<UsdPrim> textures;
    for (const UsdPrim& prim : flat->Traverse()) {
        TfToken id;
        if (UsdShadeShader(prim) && UsdShadeShader(prim).GetShaderId(&id) && id == "UsdUVTexture") {
            textures.push_back(prim);
        }
    }
    for (size_t i = 0; i < textures.size(); ++i) {
        StepWithin(0.25f, 0.55f, i, textures.size());  // stitching big tile sets is the slow part of a set
        const UsdPrim& prim = textures[i];
        const UsdAttribute file = prim.GetAttribute(kFile);
        SdfAssetPath asset;
        if (!file || !file.Get(&asset) || !UsdShadeUdimUtils::IsUdimIdentifier(asset.GetAssetPath())) continue;
        const std::string udimPath = asset.GetAssetPath();
        auto known = atlases.find(udimPath);
        if (known == atlases.end()) {
            std::string problem;
            const Atlas atlas = BuildAtlas(udimPath, flat->GetRootLayer(), atlasDir, maxTileSize, &problem);
            if (!problem.empty()) problems.push_back(problem);
            if (!atlas.path.empty()) {
                ++built;
                tilesIn += UsdShadeUdimUtils::ResolveUdimTilePaths(udimPath, flat->GetRootLayer()).size();
                if (atlas.scaled) ++scaled;
            }
            known = atlases.emplace(udimPath, atlas).first;
        }
        const Atlas& atlas = known->second;
        if (atlas.path.empty()) continue;

        // The transform goes between the texture and whatever fed its st.
        UsdShadeShader texture(prim);
        const SdfPath transformPath = prim.GetPath().GetParentPath().AppendChild(
            TfToken(prim.GetName().GetString() + "_udimAtlas"));
        UsdShadeShader transform = UsdShadeShader::Define(flat, transformPath);
        transform.CreateIdAttr(VtValue(TfToken("UsdTransform2d")));
        UsdShadeInput in = transform.CreateInput(TfToken("in"), SdfValueTypeNames->Float2);
        SdfPathVector sources;
        UsdShadeInput st = texture.GetInput(TfToken("st"));
        if (st && st.GetAttr().GetConnections(&sources) && !sources.empty()) {
            in.GetAttr().SetConnections(sources);
        } else {
            UsdShadeShader reader = UsdShadeShader::Define(
                flat, prim.GetPath().GetParentPath().AppendChild(
                          TfToken(prim.GetName().GetString() + "_udimAtlasSt")));
            reader.CreateIdAttr(VtValue(TfToken("UsdPrimvarReader_float2")));
            reader.CreateInput(TfToken("varname"), SdfValueTypeNames->String).Set(std::string("st"));
            in.ConnectToSource(reader.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2));
        }
        transform.CreateInput(TfToken("scale"), SdfValueTypeNames->Float2)
            .Set(GfVec2f(1.0f / atlas.columns, 1.0f / atlas.rows));
        transform.CreateInput(TfToken("translation"), SdfValueTypeNames->Float2)
            .Set(GfVec2f(-static_cast<float>(atlas.minColumn) / atlas.columns,
                         -static_cast<float>(atlas.minRow) / atlas.rows));
        if (!st) st = texture.CreateInput(TfToken("st"), SdfValueTypeNames->Float2);
        st.ConnectToSource(transform.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2));
        file.Set(SdfAssetPath(atlas.path));
        ++rewired;
    }

    if (built > 0) {
        rep.Info("textures", std::to_string(built) + " UDIM set(s) (" + std::to_string(tilesIn) +
                                 " tiles) stitched into one texture each, " + std::to_string(rewired) +
                                 " shader(s) rewired - Nuke does not read UDIM tile sets");
    }
    if (scaled > 0) {
        rep.Info("textures", std::to_string(scaled) + " of those had their tiles scaled down to fit (" +
                                 (maxTileSize > 0 ? "the texture cap, per tile, and " : "") + "at most " +
                                 std::to_string(kAtlasLimit) + " px per atlas)");
    }
    for (const std::string& problem : problems) {
        rep.Warn("textures", "UDIM set left as it is: " + problem);
    }
}

}  // namespace detail
}  // namespace usdprep
