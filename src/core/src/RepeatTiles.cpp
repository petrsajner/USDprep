// Repeating textures for Nuke. Nuke's USD reader does not repeat a
// texture: wrapS / wrapT "repeat" and "mirror" are ignored and wherever the
// UVs leave 0..1 the edge pixels are smeared over the surface (measured in
// 16.1 and 17.0, tools/nuke/make_wrap_probe.py). Game assets and FBX files
// tile their textures all the time - bark six times round a log came out as
// one flat grey.
//
// The fix is local to the material, like the UDIM atlas: a copy of the
// texture repeated (or mirrored) over every tile the UVs of the meshes
// bound to it reach, and a UsdTransform2d in front of the texture that
// squeezes those UVs into the copy. The meshes and their UVs are not
// touched, the originals are never written.
//
//   st' = (T(st) - (firstColumn, firstRow)) / (columns, rows)
//
// A texture set to clamp or black on an axis is not tiled on that axis -
// Nuke clamps there anyway. Unset wrap modes count as repeat: that is what
// renderers do with UVs outside 0..1, and what the asset was made for.

#include "Shared.h"

#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdShade/shader.h>

namespace usdprep {
namespace detail {

namespace {

namespace fs = std::filesystem;

// As the UDIM atlas: wider than this and GPUs (Nuke's viewer among them) refuse the texture.
constexpr int kTiledLimit = 16384;

UsdShadeShader SourceShader(const UsdShadeInput& input) {
    if (!input) return UsdShadeShader();
    UsdShadeConnectableAPI source;
    TfToken name;
    UsdShadeAttributeType type;
    if (!UsdShadeConnectableAPI::GetConnectedSource(input, &source, &name, &type)) return UsdShadeShader();
    return UsdShadeShader(source.GetPrim());
}

TfToken IdOf(const UsdShadeShader& shader) {
    TfToken id;
    if (shader) shader.GetShaderId(&id);
    return id;
}

// The value an input ends up with - its own, or the material interface's it is connected to.
VtValue ValueOf(const UsdShadeInput& input) {
    VtValue result;
    if (!input) return result;
    for (const UsdAttribute& attr : input.GetValueProducingAttributes()) {
        VtValue value;
        if (attr.Get(&value)) result = value;
    }
    return result;
}

std::string TextOf(const VtValue& value) {
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    return std::string();
}

template <typename T>
T Get(const UsdShadeShader& shader, const char* name, T fallback) {
    const VtValue value = ValueOf(shader.GetInput(TfToken(name)));
    return value.IsHolding<T>() ? value.UncheckedGet<T>() : fallback;
}

// One UsdTransform2d: scale, then rotate (degrees), then translate.
struct UvTransform {
    GfVec2f scale{1.0f, 1.0f};
    float rotation = 0.0f;
    GfVec2f translation{0.0f, 0.0f};

    GfVec2d Apply(const GfVec2d& uv) const {
        GfVec2d out(uv[0] * scale[0], uv[1] * scale[1]);
        if (rotation != 0.0f) {
            const double r = rotation * 3.14159265358979323846 / 180.0;
            out = GfVec2d(std::cos(r) * out[0] - std::sin(r) * out[1], std::sin(r) * out[0] + std::cos(r) * out[1]);
        }
        return out + GfVec2d(translation[0], translation[1]);
    }
};

struct Range {
    GfVec2d lo{std::numeric_limits<double>::max()};
    GfVec2d hi{std::numeric_limits<double>::lowest()};
    bool any = false;

    void Add(const GfVec2d& uv) {
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return;
        lo = GfVec2d(std::min(lo[0], uv[0]), std::min(lo[1], uv[1]));
        hi = GfVec2d(std::max(hi[0], uv[0]), std::max(hi[1], uv[1]));
        any = true;
    }
    void Add(const Range& other) {
        if (!other.any) return;
        Add(other.lo);
        Add(other.hi);
    }
    // The box through a transform (exact without rotation, the box around it with).
    Range Through(const UvTransform& t) const {
        Range out;
        if (!any) return out;
        out.Add(t.Apply(lo));
        out.Add(t.Apply(hi));
        out.Add(t.Apply(GfVec2d(lo[0], hi[1])));
        out.Add(t.Apply(GfVec2d(hi[0], lo[1])));
        return out;
    }
};

// A mesh, or the faces of it, a material is bound to.
struct Use {
    UsdPrim mesh;
    VtIntArray faces;  // empty = every face
};

// Which material each mesh (and each face subset) is bound to, for every
// purpose. The binding relationships are read as they are written - on the
// prim or the nearest ancestor - rather than through ComputeBoundMaterial,
// which warns about every older file that binds without applying
// MaterialBindingAPI (as MaterialsFromOutside does).
std::map<SdfPath, std::vector<Use>> UsesByMaterial(const UsdStageRefPtr& flat) {
    static const TfToken relationships[] = {TfToken("material:binding"), TfToken("material:binding:preview"),
                                            TfToken("material:binding:full")};
    std::unordered_map<SdfPath, SdfPath, SdfPath::Hash> inherited[3];
    const auto bindingOn = [&](const UsdPrim& prim, int which) {
        const UsdRelationship binding = prim.GetRelationship(relationships[which]);
        SdfPathVector targets;
        if (binding && binding.GetForwardedTargets(&targets) && !targets.empty()) return targets.front().GetPrimPath();
        return SdfPath();
    };
    const auto nearest = [&](const UsdPrim& prim, int which) {
        std::vector<SdfPath> walked;
        SdfPath bound;
        for (UsdPrim at = prim; at && !at.IsPseudoRoot(); at = at.GetParent()) {
            const auto known = inherited[which].find(at.GetPath());
            if (known != inherited[which].end()) {
                bound = known->second;
                break;
            }
            walked.push_back(at.GetPath());
            bound = bindingOn(at, which);
            if (!bound.IsEmpty()) break;
        }
        for (const SdfPath& path : walked) inherited[which][path] = bound;
        return bound;
    };

    std::map<SdfPath, std::vector<Use>> uses;
    for (const UsdPrim& prim : flat->Traverse()) {
        if (!prim.IsA<UsdGeomMesh>()) continue;
        const std::vector<UsdGeomSubset> subsets =
            UsdGeomSubset::GetGeomSubsets(UsdGeomImageable(prim), UsdGeomTokens->face, TfToken("materialBind"));
        std::vector<bool> covered;
        if (!subsets.empty()) {
            VtIntArray counts;
            UsdGeomMesh(prim).GetFaceVertexCountsAttr().Get(&counts);
            covered.assign(counts.size(), false);
        }
        for (const UsdGeomSubset& subset : subsets) {
            std::set<SdfPath> materials;
            for (int which = 0; which < 3; ++which) {
                const SdfPath path = bindingOn(subset.GetPrim(), which);
                if (!path.IsEmpty()) materials.insert(path);
            }
            VtIntArray faces;
            if (materials.empty() || !subset.GetIndicesAttr().Get(&faces)) continue;
            for (const int face : faces) {
                if (face >= 0 && static_cast<size_t>(face) < covered.size()) covered[face] = true;
            }
            for (const SdfPath& material : materials) uses[material].push_back({prim, faces});
        }
        std::set<SdfPath> materials;
        for (int which = 0; which < 3; ++which) {
            const SdfPath path = nearest(prim, which);
            if (!path.IsEmpty()) materials.insert(path);
        }
        if (materials.empty()) continue;
        VtIntArray rest;
        bool all = true;
        for (size_t face = 0; face < covered.size(); ++face) {
            if (covered[face]) {
                all = false;
            } else {
                rest.push_back(static_cast<int>(face));
            }
        }
        if (!all && rest.empty()) continue;  // every face has a subset of its own
        for (const SdfPath& material : materials) uses[material].push_back({prim, all ? VtIntArray() : rest});
    }
    return uses;
}

bool ReadUvs(const UsdGeomPrimvar& primvar, const UsdTimeCode& time, std::vector<GfVec2d>* out) {
    out->clear();
    VtVec2fArray floats;
    if (primvar.ComputeFlattened(&floats, time)) {
        for (const GfVec2f& uv : floats) out->push_back(GfVec2d(uv));
        return true;
    }
    VtVec2dArray doubles;
    if (primvar.ComputeFlattened(&doubles, time)) {
        out->assign(doubles.begin(), doubles.end());
        return true;
    }
    return false;
}

// The UVs `use` reads from the primvar `name`, every time sample.
Range RangeOf(const Use& use, const std::string& name) {
    Range range;
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(use.mesh).GetPrimvar(TfToken(name));
    if (!primvar || !primvar.HasValue()) return range;
    std::vector<UsdTimeCode> times{UsdTimeCode::Default()};
    std::vector<double> samples;
    primvar.GetAttr().GetTimeSamples(&samples);
    for (const double t : samples) times.push_back(UsdTimeCode(t));
    const UsdGeomMesh mesh(use.mesh);
    const TfToken interpolation = primvar.GetInterpolation();
    std::vector<GfVec2d> uvs;
    for (const UsdTimeCode& time : times) {
        if (!ReadUvs(primvar, time, &uvs) || uvs.empty()) continue;
        if (use.faces.empty() || interpolation == UsdGeomTokens->constant) {
            for (const GfVec2d& uv : uvs) range.Add(uv);
            continue;
        }
        VtIntArray counts, indices;
        mesh.GetFaceVertexCountsAttr().Get(&counts, time);
        mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
        std::vector<size_t> firstCorner(counts.size() + 1, 0);
        for (size_t f = 0; f < counts.size(); ++f) firstCorner[f + 1] = firstCorner[f] + std::max(0, counts[f]);
        for (const int face : use.faces) {
            if (face < 0 || static_cast<size_t>(face) >= counts.size()) continue;
            if (interpolation == UsdGeomTokens->uniform) {
                if (static_cast<size_t>(face) < uvs.size()) range.Add(uvs[face]);
                continue;
            }
            for (size_t corner = firstCorner[face]; corner < firstCorner[face + 1]; ++corner) {
                size_t at = corner;
                if (interpolation != UsdGeomTokens->faceVarying) {
                    if (corner >= indices.size()) continue;
                    at = static_cast<size_t>(std::max(0, indices[corner]));
                }
                if (at < uvs.size()) range.Add(uvs[at]);
            }
        }
    }
    return range;
}

// Which tiles an axis needs: [first, first + count).
struct Span {
    int first = 0;
    int count = 1;
    bool operator<(const Span& o) const { return std::tie(first, count) < std::tie(o.first, o.count); }
};

Span SpanOf(double lo, double hi) {
    constexpr double eps = 1e-4;  // a UV exactly on a tile edge belongs to the tile it spans
    Span span;
    span.first = static_cast<int>(std::floor(lo + eps));
    const int last = static_cast<int>(std::ceil(hi - eps));
    span.count = std::max(1, last - span.first);
    return span;
}

bool Odd(int n) { return (n % 2 + 2) % 2 == 1; }

struct Tiled {
    std::string path;  // empty = could not be made
    bool scaled = false;
};

Tiled BuildTiled(const std::string& source, const Span& u, const Span& v, bool mirrorU, bool mirrorV,
                 const fs::path& folder, int maxTileSize, std::string* problem) {
    Tiled tiled;
    const std::string name = fs::path(source).filename().string();
    const HioImageSharedPtr image =
        HioImage::OpenForReading(source, 0, 0, HioImage::SourceColorSpace::Raw, /*suppressErrors=*/true);
    if (!image) {
        *problem = "cannot read " + name;
        return tiled;
    }
    const HioFormat format = image->GetFormat();
    const int width = image->GetWidth();
    const int height = image->GetHeight();
    if (width <= 0 || height <= 0) {
        *problem = "cannot read " + name;
        return tiled;
    }
    double factor = 1.0;
    if (maxTileSize > 0) factor = std::min(factor, static_cast<double>(maxTileSize) / std::max(width, height));
    factor = std::min(factor, static_cast<double>(kTiledLimit) /
                                  std::max(static_cast<double>(width) * u.count, static_cast<double>(height) * v.count));
    tiled.scaled = factor < 1.0;
    const int tileWidth = std::max(1, static_cast<int>(width * factor));
    const int tileHeight = std::max(1, static_cast<int>(height * factor));
    const size_t pixelBytes = HioGetDataSize(format, GfVec3i(1, 1, 1));

    std::vector<unsigned char> tile(pixelBytes * tileWidth * tileHeight);
    HioImage::StorageSpec in;
    in.width = tileWidth;
    in.height = tileHeight;
    in.depth = 1;
    in.format = format;
    in.data = tile.data();
    if (!image->Read(in)) {
        *problem = "cannot read " + name;
        return tiled;
    }

    const size_t canvasWidth = static_cast<size_t>(tileWidth) * u.count;
    const size_t canvasHeight = static_cast<size_t>(tileHeight) * v.count;
    std::vector<unsigned char> canvas;
    try {
        canvas.resize(pixelBytes * canvasWidth * canvasHeight);
    } catch (const std::bad_alloc&) {
        *problem = "not enough memory for " + std::to_string(canvasWidth) + " x " + std::to_string(canvasHeight) +
                   " px of " + name;
        return tiled;
    }
    for (int row = 0; row < v.count; ++row) {
        const bool flipV = mirrorV && Odd(v.first + row);
        // image rows run top to bottom, UV rows bottom to top
        const size_t top = static_cast<size_t>(v.count - 1 - row) * tileHeight;
        for (int column = 0; column < u.count; ++column) {
            const bool flipU = mirrorU && Odd(u.first + column);
            const size_t left = static_cast<size_t>(column) * tileWidth;
            for (int y = 0; y < tileHeight; ++y) {
                const unsigned char* from = tile.data() + static_cast<size_t>(flipV ? tileHeight - 1 - y : y) * tileWidth * pixelBytes;
                unsigned char* to = canvas.data() + ((top + y) * canvasWidth + left) * pixelBytes;
                if (!flipU) {
                    std::memcpy(to, from, static_cast<size_t>(tileWidth) * pixelBytes);
                    continue;
                }
                for (int x = 0; x < tileWidth; ++x) {
                    std::memcpy(to + static_cast<size_t>(x) * pixelBytes,
                                from + static_cast<size_t>(tileWidth - 1 - x) * pixelBytes, pixelBytes);
                }
            }
        }
    }

    // Two textures that share a name live in different source folders.
    char hashed[24];
    std::snprintf(hashed, sizeof(hashed), "%08zx",
                  std::hash<std::string>{}(fs::path(source).parent_path().string()) & 0xffffffffu);
    std::error_code ec;
    fs::create_directories(folder / hashed, ec);
    std::string stem = fs::path(source).stem().string() + (mirrorU || mirrorV ? ".mirror" : ".repeat") +
                       std::to_string(u.count) + "x" + std::to_string(v.count);
    if ((mirrorU && Odd(u.first)) || (mirrorV && Odd(v.first))) stem += "odd";
    HioImage::StorageSpec spec;
    spec.width = static_cast<int>(canvasWidth);
    spec.height = static_cast<int>(canvasHeight);
    spec.depth = 1;
    spec.format = format;
    spec.data = canvas.data();
    // the source's own format where this build writes it, else PNG (8-bit) or EXR
    const HioType type = HioGetHioType(format);
    const bool eightBit = type == HioTypeUnsignedByte || type == HioTypeUnsignedByteSRGB;
    std::vector<std::string> extensions{fs::path(source).extension().string(), eightBit ? ".png" : ".exr"};
    for (const std::string& extension : extensions) {
        const fs::path out = folder / hashed / (stem + extension);
        const HioImageSharedPtr writer = HioImage::OpenForWriting(out.string());
        if (writer && writer->Write(spec)) {
            tiled.path = out.generic_string();
            return tiled;
        }
    }
    *problem = "cannot write a tiled copy of " + name + " into " + (folder / hashed).string();
    return tiled;
}

std::string UniqueChildName(const UsdStageRefPtr& stage, const SdfPath& parent, const std::string& base) {
    std::string name = base;
    for (int n = 2; stage->GetPrimAtPath(parent.AppendChild(TfToken(name))); ++n) name = base + std::to_string(n);
    return name;
}

}  // namespace

void TileRepeatingTextures(Report& rep, const UsdStageRefPtr& flat, const std::string& folder, int maxTileSize) {
    std::vector<UsdShadeShader> textures;
    for (const UsdPrim& prim : flat->Traverse()) {
        const UsdShadeShader shader(prim);
        if (shader && IdOf(shader) == "UsdUVTexture") textures.push_back(shader);
    }
    if (textures.empty()) return;
    const std::map<SdfPath, std::vector<Use>> uses = UsesByMaterial(flat);
    std::map<std::pair<SdfPath, std::string>, Range> rawRanges;  // by material and UV set
    std::map<std::tuple<std::string, Span, Span, bool, bool>, Tiled> copies;
    std::vector<std::string> problems;
    size_t rewired = 0, shifted = 0, scaled = 0;
    int widest = 0, tallest = 0;

    for (size_t i = 0; i < textures.size(); ++i) {
        StepWithin(0.55f, 0.57f, i, textures.size());
        UsdShadeShader texture = textures[i];
        const UsdShadeInput file = texture.GetInput(TfToken("file"));
        SdfAssetPath asset;
        if (!file || !file.Get(&asset)) continue;
        // a texture that is not there is reported, and taken off its material, on the way out
        const std::string source = asset.GetResolvedPath();
        if (source.empty() || UsdShadeUdimUtils::IsUdimIdentifier(source)) continue;

        const std::string wrapS = TextOf(ValueOf(texture.GetInput(TfToken("wrapS"))));
        const std::string wrapT = TextOf(ValueOf(texture.GetInput(TfToken("wrapT"))));
        const bool tileU = wrapS != "clamp" && wrapS != "black";
        const bool tileV = wrapT != "clamp" && wrapT != "black";
        if (!tileU && !tileV) continue;

        // st <- UsdTransform2d ... <- UsdPrimvarReader_float2, outermost first
        std::vector<UvTransform> chain;
        std::vector<UsdShadeShader> transforms;
        UsdShadeShader reader = SourceShader(texture.GetInput(TfToken("st")));
        std::set<SdfPath> seen;
        while (reader && IdOf(reader) == "UsdTransform2d" && seen.insert(reader.GetPath()).second) {
            UvTransform t;
            t.scale = Get<GfVec2f>(reader, "scale", GfVec2f(1.0f));
            t.rotation = Get<float>(reader, "rotation", 0.0f);
            t.translation = Get<GfVec2f>(reader, "translation", GfVec2f(0.0f));
            chain.push_back(t);
            transforms.push_back(reader);
            reader = SourceShader(reader.GetInput(TfToken("in")));
        }
        if (!reader || IdOf(reader) != "UsdPrimvarReader_float2") continue;
        const std::string uvSet = TextOf(ValueOf(reader.GetInput(TfToken("varname"))));
        if (uvSet.empty()) continue;

        // the material the texture belongs to, and the UVs of everything bound to it
        UsdPrim material = texture.GetPrim().GetParent();
        while (material && !material.IsA<UsdShadeMaterial>()) material = material.GetParent();
        if (!material) continue;
        const auto key = std::make_pair(material.GetPath(), uvSet);
        auto known = rawRanges.find(key);
        if (known == rawRanges.end()) {
            Range range;
            const auto found = uses.find(material.GetPath());
            if (found != uses.end()) {
                for (const Use& use : found->second) range.Add(RangeOf(use, uvSet));
            }
            known = rawRanges.emplace(key, range).first;
        }
        Range range = known->second;
        for (auto t = chain.rbegin(); t != chain.rend(); ++t) range = range.Through(*t);
        if (!range.any) continue;

        const Span u = tileU ? SpanOf(range.lo[0], range.hi[0]) : Span();
        const Span v = tileV ? SpanOf(range.lo[1], range.hi[1]) : Span();
        if (u.first == 0 && u.count == 1 && v.first == 0 && v.count == 1) continue;  // inside 0..1 already

        const bool mirrorU = tileU && wrapS == "mirror";
        const bool mirrorV = tileV && wrapT == "mirror";
        // repeated tiles do not care where they start; mirrored ones do, by parity
        const Span keyU{mirrorU ? (Odd(u.first) ? 1 : 0) : 0, u.count};
        const Span keyV{mirrorV ? (Odd(v.first) ? 1 : 0) : 0, v.count};
        // UVs that sit in one other tile only need moving home: the texture stays
        const bool needsCopy = u.count > 1 || v.count > 1 || keyU.first == 1 || keyV.first == 1;
        std::string tiledPath;
        if (needsCopy) {
            const auto copyKey = std::make_tuple(source, keyU, keyV, mirrorU, mirrorV);
            auto copy = copies.find(copyKey);
            if (copy == copies.end()) {
                std::string problem;
                const Tiled tiled =
                    BuildTiled(source, u, v, mirrorU, mirrorV, fs::path(folder), maxTileSize, &problem);
                if (!problem.empty()) problems.push_back(problem);
                if (!tiled.path.empty()) {
                    if (tiled.scaled) ++scaled;
                    widest = std::max(widest, u.count);
                    tallest = std::max(tallest, v.count);
                }
                copy = copies.emplace(copyKey, tiled).first;
            }
            if (copy->second.path.empty()) continue;
            tiledPath = copy->second.path;
        }

        // The squeeze goes in front of the texture. One transform of the
        // asset's own without rotation is folded into it (one node, the way
        // the UDIM atlas does it); anything else is kept and followed.
        const GfVec2f n(static_cast<float>(u.count), static_cast<float>(v.count));
        const GfVec2f first(static_cast<float>(u.first), static_cast<float>(v.first));
        UsdShadeInput st = texture.GetInput(TfToken("st"));
        SdfPathVector sources;
        GfVec2f scale(1.0f / n[0], 1.0f / n[1]);
        GfVec2f translation(-first[0] / n[0], -first[1] / n[1]);
        bool folded = false;
        if (transforms.size() == 1 && chain.front().rotation == 0.0f) {
            const UvTransform& own = chain.front();
            const UsdShadeInput ownIn = transforms.front().GetInput(TfToken("in"));
            if (ownIn && ownIn.GetAttr().GetConnections(&sources) && !sources.empty()) {
                scale = GfVec2f(own.scale[0] / n[0], own.scale[1] / n[1]);
                translation = GfVec2f((own.translation[0] - first[0]) / n[0], (own.translation[1] - first[1]) / n[1]);
                folded = true;
            }
        }
        if (!folded && (!st || !st.GetAttr().GetConnections(&sources) || sources.empty())) continue;
        const SdfPath parent = texture.GetPath().GetParentPath();
        UsdShadeShader squeeze = UsdShadeShader::Define(
            flat, parent.AppendChild(TfToken(UniqueChildName(flat, parent, texture.GetPath().GetName() + "_tiles"))));
        squeeze.CreateIdAttr(VtValue(TfToken("UsdTransform2d")));
        squeeze.CreateInput(TfToken("in"), SdfValueTypeNames->Float2).GetAttr().SetConnections(sources);
        squeeze.CreateInput(TfToken("scale"), SdfValueTypeNames->Float2).Set(scale);
        squeeze.CreateInput(TfToken("translation"), SdfValueTypeNames->Float2).Set(translation);
        st.ConnectToSource(squeeze.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2));
        if (needsCopy) {
            file.Set(SdfAssetPath(tiledPath));
            ++rewired;
        } else {
            ++shifted;
        }
    }

    size_t made = 0;
    for (const auto& copy : copies) made += copy.second.path.empty() ? 0 : 1;
    if (made > 0) {
        rep.Info("nuke", std::to_string(made) + " repeating texture(s) laid out over the tiles their UVs reach (up to " +
                             std::to_string(widest) + " x " + std::to_string(tallest) + ") for " +
                             std::to_string(rewired) +
                             " shader(s) - Nuke does not repeat a texture, it smears the edge pixels outside 0..1");
    }
    if (shifted > 0) {
        rep.Info("nuke", std::to_string(shifted) + " texture(s) read by UVs that lie in another tile moved home in "
                                                  "the material - Nuke does not repeat a texture");
    }
    if (scaled > 0) {
        rep.Info("nuke", std::to_string(scaled) + " of those had their tiles scaled down to fit (" +
                             (maxTileSize > 0 ? "the texture cap, per tile, and " : "") + "at most " +
                             std::to_string(kTiledLimit) + " px per texture)");
    }
    for (const std::string& problem : problems) {
        rep.Warn("nuke", "repeating texture left as it is, Nuke will smear its edges: " + problem);
    }
}

}  // namespace detail
}  // namespace usdprep
