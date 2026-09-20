// Export for Nukes older than the USD-based 3D system. The classic ReadGeo
// reads .obj and .abc (measured in 16.1 and 17.0 through ReadGeo +
// ScanlineRender, UVs included - NUKE_COMPAT.md): world-space meshes with
// UVs, an .obj as a still of one frame, an .abc with the animation as
// samples of the positions. Nuke reads no materials from either, so what
// it cannot take from the file it gets from a script next to it:
//
//   car.abc            everything, the file that was asked for
//   car_abc_parts/*    one file per material, when there are several -
//                      a ReadGeo takes one texture for all it reads
//   car_abc.nk         ReadGeo nodes with their textures wired in and a
//                      Scene joining them: File > Insert Comp Nodes
//
// The source is the finished .usdc the rest of the pipeline produced, so
// everything that applies to a USD export applies here too (one material
// per mesh, skinning baked, shapes meshed, textures copied and capped).

#include "Shared.h"

#include "MeshExport.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <iomanip>
#include <sstream>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/shader.h>

namespace usdprep {
namespace detail {

namespace {

namespace fs = std::filesystem;

struct Look {
    std::string name = "no_material";
    std::string texture;               // absolute path, empty = none
    GfVec3f colour = GfVec3f(0.18f);   // what Nuke shows without one
    GfVec2f uvScale = GfVec2f(1.0f);   // a UDIM atlas squeezes the UVs in the
    GfVec2f uvOffset = GfVec2f(0.0f);  // material; an .obj needs them baked
    std::string uvSet = "st";          // the primvar the texture is looked up with
    bool repeats = true;               // wrap mode of the texture: anything but clamp / black
};

// The shader an input is fed by, if any.
UsdShadeShader SourceOf(const UsdShadeInput& input) {
    if (!input) return UsdShadeShader();
    UsdShadeConnectableAPI source;
    TfToken name;
    UsdShadeAttributeType type;
    if (!UsdShadeConnectableAPI::GetConnectedSource(input, &source, &name, &type)) return UsdShadeShader();
    return UsdShadeShader(source.GetPrim());
}

Look LookOf(const UsdPrim& meshPrim) {
    Look look;
    const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(meshPrim).ComputeBoundMaterial();
    if (!material) return look;
    look.name = material.GetPrim().GetName().GetString();
    const UsdShadeShader surface = material.ComputeSurfaceSource();
    if (!surface) return look;
    const UsdShadeInput diffuse = surface.GetInput(TfToken("diffuseColor"));
    if (diffuse) diffuse.Get(&look.colour);
    const UsdShadeShader texture = SourceOf(diffuse);
    if (!texture) return look;
    SdfAssetPath file;
    if (texture.GetInput(TfToken("file")).Get(&file)) {
        look.texture = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
    }
    TfToken wrapS, wrapT;
    if (const UsdShadeInput wrap = texture.GetInput(TfToken("wrapS"))) wrap.Get(&wrapS);
    if (const UsdShadeInput wrap = texture.GetInput(TfToken("wrapT"))) wrap.Get(&wrapT);
    look.repeats = wrapS != "clamp" && wrapS != "black" && wrapT != "clamp" && wrapT != "black";
    // Which UVs the texture is looked up with: whatever feeds its st - a
    // primvar reader, possibly through a UsdTransform2d (our UDIM atlas).
    // Production assets do not always call it "st" (ALab's preview
    // materials read "perfuv").
    UsdShadeShader reader = SourceOf(texture.GetInput(TfToken("st")));
    TfToken id;
    if (reader && reader.GetShaderId(&id) && id == "UsdTransform2d") {
        if (const UsdShadeInput scale = reader.GetInput(TfToken("scale"))) scale.Get(&look.uvScale);
        if (const UsdShadeInput offset = reader.GetInput(TfToken("translation"))) offset.Get(&look.uvOffset);
        reader = SourceOf(reader.GetInput(TfToken("in")));
    }
    if (reader && reader.GetShaderId(&id) && id == "UsdPrimvarReader_float2") {
        if (const UsdShadeInput varname = reader.GetInput(TfToken("varname"))) {
            // the name may come from the material's interface; it is a string in newer files, a token in older
            for (const UsdAttribute& attr : varname.GetValueProducingAttributes()) {
                VtValue value;
                if (!attr.Get(&value)) continue;
                if (value.IsHolding<std::string>()) look.uvSet = value.UncheckedGet<std::string>();
                if (value.IsHolding<TfToken>()) look.uvSet = value.UncheckedGet<TfToken>().GetString();
            }
        }
    }
    return look;
}

bool ReadMesh(const UsdPrim& prim, const UsdTimeCode& time, UsdGeomXformCache& xforms, const Look& look,
              MeshData* out) {
    const UsdGeomMesh mesh(prim);
    if (!mesh.GetPointsAttr().Get(&out->points, time) || out->points.empty()) return false;
    if (!mesh.GetFaceVertexCountsAttr().Get(&out->counts, time) ||
        !mesh.GetFaceVertexIndicesAttr().Get(&out->indices, time)) {
        return false;
    }
    const GfMatrix4d toWorld = xforms.GetLocalToWorldTransform(prim);
    for (GfVec3f& p : out->points) p = GfVec3f(toWorld.Transform(GfVec3d(p)));
    TfToken orientation;
    out->flip = mesh.GetOrientationAttr().Get(&orientation) && orientation == UsdGeomTokens->leftHanded;
    out->name = prim.GetPath().GetString().substr(1);

    UsdGeomPrimvar st = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(look.uvSet.empty() ? "st" : look.uvSet));
    if (!st) st = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("st"));
    VtVec2fArray values;
    if (st && st.ComputeFlattened(&values, time) && !values.empty()) {
        const TfToken interpolation = st.GetInterpolation();
        out->uvs.resize(out->indices.size());
        for (size_t corner = 0; corner < out->indices.size(); ++corner) {
            size_t at = 0;
            if (interpolation == UsdGeomTokens->faceVarying) {
                at = corner;
            } else if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) {
                at = static_cast<size_t>(out->indices[corner]);
            }
            const GfVec2f uv = at < values.size() ? values[at] : GfVec2f(0.0f);
            out->uvs[corner] = GfVec2f(uv[0] * look.uvScale[0] + look.uvOffset[0],
                                       uv[1] * look.uvScale[1] + look.uvOffset[1]);
        }
        // A renderer repeats a texture outside 0..1; Nuke's classic 3D shows
        // black there. UVs laid out in other tiles (a UDIM layout under a
        // single preview texture) are brought home face by face - each
        // corner has its own UV here, so a whole face can move by whole tiles.
        if (look.repeats) {
            size_t corner = 0;
            for (const int count : out->counts) {
                GfVec2f low(std::numeric_limits<float>::max());
                for (int k = 0; k < count; ++k) {
                    low[0] = std::min(low[0], out->uvs[corner + k][0]);
                    low[1] = std::min(low[1], out->uvs[corner + k][1]);
                }
                // a face that starts exactly on a tile edge belongs to the tile it spans
                const GfVec2f shift(std::floor(low[0] + 1e-5f), std::floor(low[1] + 1e-5f));
                if (shift != GfVec2f(0.0f)) {
                    for (int k = 0; k < count; ++k) out->uvs[corner + k] -= shift;
                }
                corner += static_cast<size_t>(count);
            }
        }
    }
    return true;
}

// Appends one mesh; `vertexBase` / `uvBase` are the running 1-based offsets.
void WriteMesh(std::ostream& os, const MeshData& mesh, size_t* vertexBase, size_t* uvBase) {
    os << "o " << mesh.name << "\n";
    for (const GfVec3f& p : mesh.points) os << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    for (const GfVec2f& uv : mesh.uvs) os << "vt " << uv[0] << " " << uv[1] << "\n";
    size_t corner = 0;
    for (const int count : mesh.counts) {
        if (count >= 3) {
            os << "f";
            for (int k = 0; k < count; ++k) {
                const size_t c = corner + (mesh.flip ? count - 1 - k : k);
                os << " " << (*vertexBase + mesh.indices[c]);
                if (!mesh.uvs.empty()) os << "/" << (*uvBase + c);
            }
            os << "\n";
        }
        corner += static_cast<size_t>(count);
    }
    *vertexBase += mesh.points.size();
    *uvBase += mesh.uvs.size();
}

std::string NukePath(const fs::path& path) { return fs::absolute(path).generic_string(); }

// A name a Nuke node can carry.
std::string NodeName(const std::string& text) {
    std::string out;
    for (const char c : text) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? "geo" : out;
}

}  // namespace

bool ExportMeshFile(Report& rep, const std::string& usdPath, const std::string& outPath, double frame) {
    const UsdStageRefPtr stage = UsdStage::Open(usdPath);
    if (!stage) {
        rep.Fail("cannot reopen the prepared scene for the export: " + usdPath);
        return false;
    }
    const bool asAbc = HasExtension(outPath, ".abc");
    const char* tag = asAbc ? "abc" : "obj";
    const std::string extension = asAbc ? ".abc" : ".obj";
    const double first = std::isnan(frame) ? stage->GetStartTimeCode() : frame;

    // meshes by look, in scene order; hidden ones (kept proxies) stay out
    struct Item {
        UsdPrim prim;
        size_t look;
    };
    std::vector<Look> looks;
    std::vector<Item> items;
    std::vector<AbcMesh> meshes;
    bool animated = false;
    // instanced content is geometry like any other here: the files have no instancing to keep
    for (const UsdPrim& prim : UsdPrimRange(stage->GetPseudoRoot(), UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        if (!prim.IsA<UsdGeomMesh>()) continue;
        if (UsdGeomImageable(prim).ComputeVisibility(UsdTimeCode(first)) == UsdGeomTokens->invisible) continue;
        VtVec3fArray points;
        if (!UsdGeomMesh(prim).GetPointsAttr().Get(&points, UsdTimeCode(first)) || points.empty()) continue;
        const Look look = LookOf(prim);
        size_t slot = 0;
        for (; slot < looks.size(); ++slot) {
            if (looks[slot].name == look.name) break;
        }
        if (slot == looks.size()) looks.push_back(look);
        bool moves = UsdGeomMesh(prim).GetPointsAttr().GetNumTimeSamples() > 1;
        for (UsdPrim p = prim; p && !p.IsPseudoRoot() && !moves; p = p.GetParent()) {
            const UsdGeomXformable xformable(p);
            moves = xformable && xformable.TransformMightBeTimeVarying();
        }
        animated = animated || moves;
        items.push_back({prim, slot});
        meshes.push_back({prim.GetPath().GetString().substr(1), moves});
    }
    if (items.empty()) {
        rep.Fail("nothing to write: the selection has no mesh geometry");
        return false;
    }

    // An .obj is one frame. An .abc carries the whole range, unless one
    // frame was asked for or nothing moves.
    std::vector<double> frames{first};
    if (asAbc && animated && std::isnan(frame)) {
        frames.clear();
        for (double f = std::ceil(stage->GetStartTimeCode()); f <= std::floor(stage->GetEndTimeCode()); f += 1.0) {
            frames.push_back(f);
        }
        if (frames.empty()) frames.push_back(first);
    }

    const MeshSampler sampler = [&](size_t index, double at, MeshData* out) {
        UsdGeomXformCache xforms{UsdTimeCode(at)};
        return ReadMesh(items[index].prim, UsdTimeCode(at), xforms, looks[items[index].look], out);
    };
    // A ReadGeo created by a script loads the first object of an .abc only,
    // unless its scene_view knob lists them all (measured) - so the .nk does.
    std::map<std::string, std::vector<std::string>> objectsIn;
    const auto writeFile = [&](const fs::path& path, const std::vector<size_t>& which) {
        if (asAbc) {
            std::string error;
            if (WriteAlembic(path.string(), meshes, which, frames, stage->GetTimeCodesPerSecond(), sampler,
                             &objectsIn[path.string()], &error)) {
                return true;
            }
            rep.Fail("cannot write " + path.string() + ": " + error);
            return false;
        }
        std::ofstream os(path, std::ios::binary);
        os << "# USDprep - world space, frame " << FormatFrame(first) << "\n";
        os << std::setprecision(7);  // what a float holds; more only prints its noise
        size_t vertexBase = 1, uvBase = 1;
        size_t group = static_cast<size_t>(-1);
        for (const size_t index : which) {
            MeshData mesh;
            if (!sampler(index, first, &mesh)) continue;
            if (items[index].look != group) {
                group = items[index].look;
                os << "g " << looks[group].name << "\n";
            }
            WriteMesh(os, mesh, &vertexBase, &uvBase);
        }
        if (!os.good()) rep.Fail("cannot write " + path.string());
        return os.good();
    };

    // everything, grouped by look - then one file per material when there are several
    const fs::path out(outPath);
    std::vector<std::vector<size_t>> byLook(looks.size());
    std::vector<size_t> all;
    for (size_t i = 0; i < items.size(); ++i) byLook[items[i].look].push_back(i);
    for (const std::vector<size_t>& group : byLook) all.insert(all.end(), group.begin(), group.end());
    if (!writeFile(out, all)) return false;

    // "car_abc.nk", "car_abc_parts": an .obj and an .abc of the same name do not take each other's
    const std::string sidecar = out.stem().string() + "_" + extension.substr(1);
    std::vector<fs::path> partOf(looks.size(), out);
    if (looks.size() > 1) {
        const fs::path parts = out.parent_path() / (sidecar + "_parts");
        std::error_code ec;
        fs::create_directories(parts, ec);
        for (size_t slot = 0; slot < looks.size(); ++slot) {
            partOf[slot] = parts / (NodeName(looks[slot].name) + extension);
            if (!writeFile(partOf[slot], byLook[slot])) return false;
        }
    }

    // the script that puts it together in Nuke
    const fs::path script = out.parent_path() / (sidecar + ".nk");
    std::ofstream nk(script, std::ios::binary);
    size_t textured = 0;
    for (size_t slot = 0; slot < looks.size(); ++slot) {
        const Look& look = looks[slot];
        const std::string name = NodeName(look.name);
        if (!look.texture.empty()) {
            nk << "Read {\n inputs 0\n file \"" << NukePath(look.texture) << "\"\n name tex_" << name << "\n}\n";
            ++textured;
        } else {
            nk << "Constant {\n inputs 0\n color {" << look.colour[0] << " " << look.colour[1] << " " << look.colour[2]
               << " 1}\n name colour_" << name << "\n}\n";
        }
        nk << "ReadGeo2 {\n file \"" << NukePath(partOf[slot]) << "\"\n";
        const std::vector<std::string>& objects = objectsIn[partOf[slot].string()];
        if (!objects.empty()) {
            std::string numbers, paths;
            for (size_t i = 0; i < objects.size(); ++i) {
                numbers += " " + std::to_string(i);
                paths += " " + objects[i];
            }
            nk << " scene_view {{0} imported:" << numbers << " selected:" << numbers << " items:" << paths << "}\n";
        }
        nk << " name geo_" << name << "\n}\n";
    }
    nk << "Scene {\n inputs " << looks.size() << "\n name " << NodeName(out.stem().string()) << "\n}\n";
    nk.close();

    if (frames.size() > 1) {
        rep.Info(tag, std::to_string(items.size()) + " mesh(es) written in world space, frames " +
                          FormatFrame(frames.front()) + "-" + FormatFrame(frames.back()) + " at " +
                          FormatFrame(stage->GetTimeCodesPerSecond()) +
                          " fps - for Nuke's classic 3D (ReadGeo); set the project to the same frame rate");
    } else {
        rep.Info(tag, std::to_string(items.size()) + " mesh(es) written in world space at frame " +
                          FormatFrame(first) + " - for Nuke's classic 3D (ReadGeo)");
    }
    if (animated && !asAbc) {
        rep.Warn(tag, "the selection is animated; an .obj is a still of frame " + FormatFrame(first) +
                          " - an .abc carries the animation");
    }
    rep.Info(tag, script.filename().string() + ": " + std::to_string(looks.size()) + " ReadGeo node(s), " +
                      std::to_string(textured) + " with a texture wired in - Nuke reads no materials from an " +
                      extension + ". File > Insert Comp Nodes; the paths inside are absolute" +
                      (looks.size() > 1 ? ", the per-material files are in " + sidecar + "_parts" : ""));
    return true;
}

}  // namespace detail
}  // namespace usdprep
