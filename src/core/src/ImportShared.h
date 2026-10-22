// What the importers (ObjImport.cpp, AbcImport.cpp, FbxImport.cpp) share:
// paths, names, numbers for the report, and the preview material every
// format's materials are written as.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>

#include <usdprep/Progress.h>
#include <usdprep/Report.h>

namespace usdprep {
namespace importer {

namespace fs = std::filesystem;

// --------------------------------------------------------------- the formats
Report ImportObj(const std::string& inputPath, const std::string& usdPath, Progress* progress);
Report ImportAbc(const std::string& inputPath, const std::string& usdPath, Progress* progress);
Report ImportFbx(const std::string& inputPath, const std::string& usdPath, Progress* progress);

// ----------------------------------------------------------------- small tools
inline fs::path PathOf(const std::string& utf8) { return fs::u8path(utf8); }
inline std::string Utf8Of(const fs::path& path) { return path.u8string(); }

std::string Lower(std::string text);
bool EndsWithNoCase(const std::string& text, const char* suffix);
FILE* OpenForReading(const std::string& utf8);
bool ReadWholeFile(const std::string& utf8, std::string* out);
bool IsFile(const fs::path& path);

// 1234567 -> "1,234,567": the report is read by people.
std::string Count(size_t n);
// 12.5000 -> "12.5"
std::string Number(double value);

// Prim names: letters, digits and underscores. Accented Latin letters lose
// their accents ("Předávka" -> "Predavka") rather than turning into
// underscores; anything else becomes an underscore.
std::string PrimNameFor(const std::string& utf8, const std::string& fallback);
// `wanted`, or wanted_2, wanted_3... - whichever is still free.
std::string UniqueName(const std::string& wanted, std::unordered_map<std::string, int>* taken);

// What a reader library's exception says, for a person: Alembic's read
// "IArchive::IArchive( iFileName )", "ERROR: EXCEPTION:", "bad allocation"
// on three lines - the last one is what went wrong.
std::string ReadableError(const std::string& what);

// Notes in the layer which file the scene was converted from (kImportedFromKey).
void MarkImported(const pxr::UsdStageRefPtr& stage, const fs::path& source);

// Writes the converted stage and fills the report's output fields.
bool SaveImported(const pxr::UsdStageRefPtr& stage, const std::string& usdPath, Report& rep);

// ----------------------------------------------------------- preview material
// One texture of a material: the file as found (or as written, when it is
// not there - the export then names it), which channel feeds a single
// value, and the UV placement of the map.
struct MapRef {
    std::string file;
    std::string channel;  // "r" "g" "b" "a"; empty = the natural one (alpha for an opacity map in the colour file)
    pxr::GfVec2f scale{1.0f, 1.0f};
    pxr::GfVec2f offset{0.0f, 0.0f};
    bool invert = false;  // one minus the value: a glossiness map read as roughness
    bool Set() const { return !file.empty(); }
};

// A material as UsdPreviewSurface reads it. A map wins over the value.
struct PreviewMaterial {
    bool hasDiffuse = false;
    pxr::GfVec3f diffuse{0.8f};
    MapRef diffuseMap;
    bool hasOpacity = false;
    float opacity = 1.0f;
    MapRef opacityMap;
    bool hasRoughness = false;
    float roughness = 0.5f;
    MapRef roughnessMap;
    bool hasMetallic = false;
    float metallic = 0.0f;
    MapRef metallicMap;
    bool hasEmissive = false;
    pxr::GfVec3f emissive{0.0f};
    MapRef emissiveMap;
    MapRef normalMap;  // tangent-space normal map
    std::string uvSet = "st";
};

// Writes Material + UsdPreviewSurface + the texture nodes it needs.
class PreviewMaterialWriter {
public:
    explicit PreviewMaterialWriter(const pxr::UsdStageRefPtr& stage) : _stage(stage) {}
    pxr::UsdShadeMaterial Write(const pxr::SdfPath& path, const PreviewMaterial& material);
    size_t textures = 0;  // texture nodes written, all materials together

private:
    pxr::UsdShadeOutput StReader(const std::string& uvSet);
    pxr::UsdShadeShader Texture(const std::string& nodeName, const MapRef& map, bool data, const std::string& uvSet);
    void Connect(const char* input, const pxr::SdfValueTypeName& inputType, pxr::UsdShadeShader source,
                 const std::string& output, const pxr::SdfValueTypeName& outputType);

    pxr::UsdStageRefPtr _stage;
    pxr::SdfPath _path;
    pxr::UsdShadeShader _surface;
    pxr::UsdShadeOutput _reader;
};

}  // namespace importer
}  // namespace usdprep
