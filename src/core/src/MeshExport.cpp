// Export for Nukes older than the USD-based 3D system: the classic ReadGeo
// reads .obj (measured in 16.1 and 17.0 through ReadGeo + ScanlineRender,
// UVs included - NUKE_COMPAT.md). An .obj is a still: world-space points
// at one frame, UVs, no materials that Nuke would read. What Nuke cannot
// take from the file it gets from a script next to it:
//
//   car.obj            everything, the file that was asked for
//   car_parts/*.obj    one file per material, when there are several -
//                      a ReadGeo takes one texture for all it reads
//   car.nk             ReadGeo nodes with their textures wired in and a
//                      Scene joining them: File > Insert Comp Nodes
//
// The source is the finished .usdc the rest of the pipeline produced, so
// everything that applies to a USD export applies here too (one material
// per mesh, skinning baked, shapes meshed, textures copied and capped).

#include "Shared.h"

#include <fstream>
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
    const UsdShadeShader transform = SourceOf(texture.GetInput(TfToken("st")));
    TfToken id;
    if (transform && transform.GetShaderId(&id) && id == "UsdTransform2d") {
        if (const UsdShadeInput scale = transform.GetInput(TfToken("scale"))) scale.Get(&look.uvScale);
        if (const UsdShadeInput offset = transform.GetInput(TfToken("translation"))) offset.Get(&look.uvOffset);
    }
    return look;
}

struct MeshData {
    std::string name;
    VtVec3fArray points;   // world space
    VtVec2fArray uvs;      // one per corner, empty = none
    VtIntArray counts, indices;
    bool flip = false;     // leftHanded: reverse the winding
};

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

    const UsdGeomPrimvar st = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("st"));
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

bool ExportObj(Report& rep, const std::string& usdPath, const std::string& objPath, double frame) {
    const UsdStageRefPtr stage = UsdStage::Open(usdPath);
    if (!stage) {
        rep.Fail("cannot reopen the prepared scene for the .obj export: " + usdPath);
        return false;
    }
    const UsdTimeCode time = std::isnan(frame) ? UsdTimeCode(stage->GetStartTimeCode()) : UsdTimeCode(frame);
    UsdGeomXformCache xforms(time);

    // meshes by look, in scene order; hidden ones (kept proxies) stay out
    std::vector<Look> looks;
    std::vector<std::vector<MeshData>> meshesOf;
    size_t meshCount = 0;
    bool animated = false;
    for (const UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<UsdGeomMesh>()) continue;
        if (UsdGeomImageable(prim).ComputeVisibility(time) == UsdGeomTokens->invisible) continue;
        const Look look = LookOf(prim);
        MeshData mesh;
        if (!ReadMesh(prim, time, xforms, look, &mesh)) continue;
        if (UsdGeomMesh(prim).GetPointsAttr().GetNumTimeSamples() > 1 || xforms.TransformMightBeTimeVarying(prim)) {
            animated = true;
        }
        size_t slot = 0;
        for (; slot < looks.size(); ++slot) {
            if (looks[slot].name == look.name) break;
        }
        if (slot == looks.size()) {
            looks.push_back(look);
            meshesOf.emplace_back();
        }
        meshesOf[slot].push_back(std::move(mesh));
        ++meshCount;
    }
    if (meshCount == 0) {
        rep.Fail("nothing to write: the selection has no mesh geometry");
        return false;
    }

    const auto writeFile = [&](const fs::path& path, const std::vector<size_t>& slots) {
        std::ofstream os(path, std::ios::binary);
        if (!os) return false;
        os << "# USDprep - world space, frame " << FormatFrame(time.GetValue()) << "\n";
        os << std::setprecision(7);  // what a float holds; more only prints its noise
        size_t vertexBase = 1, uvBase = 1;
        for (const size_t slot : slots) {
            os << "g " << looks[slot].name << "\n";
            for (const MeshData& mesh : meshesOf[slot]) WriteMesh(os, mesh, &vertexBase, &uvBase);
        }
        return os.good();
    };

    const fs::path obj(objPath);
    std::vector<size_t> all(looks.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    if (!writeFile(obj, all)) {
        rep.Fail("cannot write " + objPath);
        return false;
    }

    // one file per material when there are several
    std::vector<fs::path> partOf(looks.size(), obj);
    if (looks.size() > 1) {
        const fs::path parts = obj.parent_path() / (obj.stem().string() + "_parts");
        std::error_code ec;
        fs::create_directories(parts, ec);
        for (size_t slot = 0; slot < looks.size(); ++slot) {
            partOf[slot] = parts / (NodeName(looks[slot].name) + ".obj");
            if (!writeFile(partOf[slot], {slot})) {
                rep.Fail("cannot write " + partOf[slot].string());
                return false;
            }
        }
    }

    // the script that puts it together in Nuke
    const fs::path script = obj.parent_path() / (obj.stem().string() + ".nk");
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
        nk << "ReadGeo2 {\n file \"" << NukePath(partOf[slot]) << "\"\n name geo_" << name << "\n}\n";
    }
    nk << "Scene {\n inputs " << looks.size() << "\n name " << NodeName(obj.stem().string()) << "\n}\n";
    nk.close();

    rep.Info("obj", std::to_string(meshCount) + " mesh(es) written in world space at frame " +
                        FormatFrame(time.GetValue()) + " - for Nuke's classic 3D (ReadGeo)");
    if (animated) {
        rep.Warn("obj", "the selection is animated; an .obj is a still of frame " + FormatFrame(time.GetValue()));
    }
    rep.Info("obj", script.filename().string() + ": " + std::to_string(looks.size()) + " ReadGeo node(s), " +
                        std::to_string(textured) +
                        " with a texture wired in - Nuke reads no materials from an .obj. "
                        "File > Insert Comp Nodes; the paths inside are absolute" +
                        (looks.size() > 1 ? ", the per-material files are in " + obj.stem().string() + "_parts" : ""));
    return true;
}

}  // namespace detail
}  // namespace usdprep
