#include "ImportShared.h"

#include <cctype>
#include <cstring>

#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdprep {
namespace importer {

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool EndsWithNoCase(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && Lower(text.substr(text.size() - n)) == suffix;
}

FILE* OpenForReading(const std::string& utf8) {
#ifdef _WIN32
    return _wfopen(PathOf(utf8).c_str(), L"rb");
#else
    return std::fopen(utf8.c_str(), "rb");
#endif
}

bool ReadWholeFile(const std::string& utf8, std::string* out) {
    FILE* file = OpenForReading(utf8);
    if (!file) return false;
    char buffer[1 << 16];
    size_t n = 0;
    out->clear();
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out->append(buffer, n);
    std::fclose(file);
    return true;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::string Count(size_t n) {
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

std::string Number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

namespace {
const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOO_OUUUUYTsaaaaaaaceeeeiiiidnooooo_ouuuuyty";  // U+00C0..U+00FF
const char kLatinExtA[] =                                                                   // U+0100..U+017F
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static_assert(sizeof(kLatin1) == 0x40 + 1, "one letter per code point U+00C0..U+00FF");
static_assert(sizeof(kLatinExtA) == 0x80 + 1, "one letter per code point U+0100..U+017F");
}  // namespace

std::string PrimNameFor(const std::string& utf8, const std::string& fallback) {
    std::string name;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            name += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
            ++i;
            continue;
        }
        // one UTF-8 sequence
        const size_t length = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        unsigned int code = 0;
        if (length == 2 && i + 1 < utf8.size()) code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
        if (code >= 0xC0 && code < 0x100) {
            name += kLatin1[code - 0xC0];
        } else if (code >= 0x100 && code < 0x180) {
            name += kLatinExtA[code - 0x100];
        } else {
            name += '_';
        }
        i += length;
    }
    // no leading, trailing or doubled underscores left over from spaces, dots and colons
    std::string tidy;
    for (const char c : name) {
        if (c == '_' && (tidy.empty() || tidy.back() == '_')) continue;
        tidy += c;
    }
    while (!tidy.empty() && tidy.back() == '_') tidy.pop_back();
    if (tidy.empty()) tidy = fallback;
    if (std::isdigit(static_cast<unsigned char>(tidy[0]))) tidy = "_" + tidy;
    return tidy;
}

std::string ReadableError(const std::string& what) {
    const size_t at = what.rfind("EXCEPTION:");
    if (at == std::string::npos) return what;
    std::string reason = what.substr(at + 10);
    const size_t first = reason.find_first_not_of(" \t\r\n");
    const size_t last = reason.find_last_not_of(" \t\r\n");
    reason = first == std::string::npos ? std::string() : reason.substr(first, last - first + 1);
    if (reason == "bad allocation") reason = "what it says about itself would need more memory than there is";
    return "the file is damaged" + (reason.empty() ? std::string() : " (" + reason + ")");
}

std::string UniqueName(const std::string& wanted, std::unordered_map<std::string, int>* taken) {
    std::string name = wanted;
    for (int n = 2; (*taken)[name]++ > 0; ++n) name = wanted + "_" + std::to_string(n);
    return name;
}

void MarkImported(const UsdStageRefPtr& stage, const fs::path& source) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(source, ec);
    VtDictionary data = stage->GetRootLayer()->GetCustomLayerData();
    data["usdprep:importedFrom"] = VtValue(Utf8Of(ec || absolute.empty() ? source : absolute));
    stage->GetRootLayer()->SetCustomLayerData(data);
}

bool SaveImported(const UsdStageRefPtr& stage, const std::string& usdPath, Report& rep) {
    const fs::path target = PathOf(usdPath);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (!stage->GetRootLayer()->Export(usdPath)) {
        rep.Fail("cannot write " + usdPath);
        return false;
    }
    rep.outputSizeBytes = fs::file_size(target, ec);
    return true;
}

// ------------------------------------------------------------ preview material

UsdShadeMaterial PreviewMaterialWriter::Write(const SdfPath& path, const PreviewMaterial& m) {
    _path = path;
    _reader = UsdShadeOutput();
    UsdShadeMaterial material = UsdShadeMaterial::Define(_stage, path);
    _surface = UsdShadeShader::Define(_stage, path.AppendChild(TfToken("PreviewSurface")));
    _surface.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    material.CreateSurfaceOutput().ConnectToSource(_surface.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token));

    UsdShadeShader diffuse;
    if (m.diffuseMap.Set()) {
        diffuse = Texture("diffuseTexture", m.diffuseMap, false, m.uvSet);
        Connect("diffuseColor", SdfValueTypeNames->Color3f, diffuse, "rgb", SdfValueTypeNames->Float3);
        // the colour underneath: what shows when the texture is not there
        if (m.hasDiffuse) _surface.GetInput(TfToken("diffuseColor")).Set(m.diffuse);
    } else if (m.hasDiffuse) {
        _surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(m.diffuse);
    }

    if (m.opacityMap.Set()) {
        // the colour texture's own alpha, or a mask of its own
        const bool sameAsColor = diffuse && m.opacityMap.file == m.diffuseMap.file;
        const std::string channel = !m.opacityMap.channel.empty() ? m.opacityMap.channel : sameAsColor ? "a" : "r";
        Connect("opacity", SdfValueTypeNames->Float,
                sameAsColor ? diffuse : Texture("opacityTexture", m.opacityMap, true, m.uvSet), channel,
                SdfValueTypeNames->Float);
        if (m.hasOpacity) _surface.GetInput(TfToken("opacity")).Set(m.opacity);
    } else if (m.hasOpacity && m.opacity < 1.0f) {
        _surface.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(m.opacity);
    }

    if (m.roughnessMap.Set()) {
        Connect("roughness", SdfValueTypeNames->Float, Texture("roughnessTexture", m.roughnessMap, true, m.uvSet),
                m.roughnessMap.channel.empty() ? "r" : m.roughnessMap.channel, SdfValueTypeNames->Float);
        if (m.hasRoughness) _surface.GetInput(TfToken("roughness")).Set(m.roughness);
    } else if (m.hasRoughness) {
        _surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(m.roughness);
    }

    if (m.metallicMap.Set()) {
        Connect("metallic", SdfValueTypeNames->Float, Texture("metallicTexture", m.metallicMap, true, m.uvSet),
                m.metallicMap.channel.empty() ? "r" : m.metallicMap.channel, SdfValueTypeNames->Float);
        if (m.hasMetallic) _surface.GetInput(TfToken("metallic")).Set(m.metallic);
    } else if (m.hasMetallic) {
        _surface.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(m.metallic);
    }

    if (m.emissiveMap.Set()) {
        Connect("emissiveColor", SdfValueTypeNames->Color3f, Texture("emissiveTexture", m.emissiveMap, false, m.uvSet),
                "rgb", SdfValueTypeNames->Float3);
    } else if (m.hasEmissive && (m.emissive[0] > 0.0f || m.emissive[1] > 0.0f || m.emissive[2] > 0.0f)) {
        _surface.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set(m.emissive);
    }

    if (m.normalMap.Set()) {
        UsdShadeShader texture = Texture("normalTexture", m.normalMap, true, m.uvSet);
        texture.CreateInput(TfToken("scale"), SdfValueTypeNames->Float4).Set(GfVec4f(2.0f, 2.0f, 2.0f, 1.0f));
        texture.CreateInput(TfToken("bias"), SdfValueTypeNames->Float4).Set(GfVec4f(-1.0f, -1.0f, -1.0f, 0.0f));
        Connect("normal", SdfValueTypeNames->Normal3f, texture, "rgb", SdfValueTypeNames->Float3);
    }
    return material;
}

UsdShadeOutput PreviewMaterialWriter::StReader(const std::string& uvSet) {
    if (!_reader) {
        UsdShadeShader reader = UsdShadeShader::Define(_stage, _path.AppendChild(TfToken("stReader")));
        reader.CreateIdAttr(VtValue(TfToken("UsdPrimvarReader_float2")));
        reader.CreateInput(TfToken("varname"), SdfValueTypeNames->Token).Set(TfToken(uvSet.empty() ? "st" : uvSet));
        _reader = reader.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2);
    }
    return _reader;
}

UsdShadeShader PreviewMaterialWriter::Texture(const std::string& nodeName, const MapRef& map, bool data,
                                              const std::string& uvSet) {
    UsdShadeShader texture = UsdShadeShader::Define(_stage, _path.AppendChild(TfToken(nodeName)));
    texture.CreateIdAttr(VtValue(TfToken("UsdUVTexture")));
    texture.CreateInput(TfToken("file"), SdfValueTypeNames->Asset).Set(SdfAssetPath(map.file));
    UsdShadeOutput st = StReader(uvSet);
    if (map.scale != GfVec2f(1.0f, 1.0f) || map.offset != GfVec2f(0.0f, 0.0f)) {
        UsdShadeShader transform = UsdShadeShader::Define(_stage, _path.AppendChild(TfToken(nodeName + "_transform")));
        transform.CreateIdAttr(VtValue(TfToken("UsdTransform2d")));
        transform.CreateInput(TfToken("in"), SdfValueTypeNames->Float2).ConnectToSource(st);
        transform.CreateInput(TfToken("scale"), SdfValueTypeNames->Float2).Set(map.scale);
        transform.CreateInput(TfToken("translation"), SdfValueTypeNames->Float2).Set(map.offset);
        st = transform.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2);
    }
    texture.CreateInput(TfToken("st"), SdfValueTypeNames->Float2).ConnectToSource(st);
    texture.CreateInput(TfToken("wrapS"), SdfValueTypeNames->Token).Set(TfToken("repeat"));
    texture.CreateInput(TfToken("wrapT"), SdfValueTypeNames->Token).Set(TfToken("repeat"));
    if (data) texture.CreateInput(TfToken("sourceColorSpace"), SdfValueTypeNames->Token).Set(TfToken("raw"));
    if (map.invert) {
        texture.CreateInput(TfToken("scale"), SdfValueTypeNames->Float4).Set(GfVec4f(-1.0f, -1.0f, -1.0f, 1.0f));
        texture.CreateInput(TfToken("bias"), SdfValueTypeNames->Float4).Set(GfVec4f(1.0f, 1.0f, 1.0f, 0.0f));
    }
    ++textures;
    return texture;
}

void PreviewMaterialWriter::Connect(const char* input, const SdfValueTypeName& inputType, UsdShadeShader source,
                                    const std::string& output, const SdfValueTypeName& outputType) {
    _surface.CreateInput(TfToken(input), inputType).ConnectToSource(source.CreateOutput(TfToken(output), outputType));
}

}  // namespace importer
}  // namespace usdprep
