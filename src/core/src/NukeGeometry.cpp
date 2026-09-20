// Geometry Nuke does not draw the way a 3D application would, converted
// into geometry it does (measured: NUKE_COMPAT.md):
//
// - Per-face materials. Nuke ignores material bindings on GeomSubsets and
//   shows the whole mesh grey. The mesh becomes a group of the same name
//   and every material subset a mesh of the subset's name inside it - so
//   "/Car/Body/Chrome" is still where the chrome binding lives.
// - Implicit shapes (Sphere, Cube, Cylinder, Cone, Capsule) are not drawn
//   at all. They become meshes, tessellated by USD's own generators.
// - BasisCurves and Volumes are not drawn either; there is no faithful
//   cheap substitute, so they are only named in the report.

#include "Shared.h"

#include <numeric>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/geomUtil/capsuleMeshGenerator.h>
#include <pxr/imaging/geomUtil/coneMeshGenerator.h>
#include <pxr/imaging/geomUtil/cuboidMeshGenerator.h>
#include <pxr/imaging/geomUtil/cylinderMeshGenerator.h>
#include <pxr/imaging/geomUtil/sphereMeshGenerator.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>

namespace usdprep {
namespace detail {

namespace {

std::string NameList(const std::vector<std::string>& names, size_t limit = 5) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < limit; ++i) out += (i ? ", " : "") + names[i];
    if (names.size() > limit) out += " and " + std::to_string(names.size() - limit) + " more";
    return out;
}

// ----------------------------------------------------------- subset split

template <typename T>
bool TakeAs(const VtValue& in, const std::vector<int>& take, size_t stride, VtValue* out) {
    if (!in.IsHolding<VtArray<T>>()) return false;
    const VtArray<T>& src = in.UncheckedGet<VtArray<T>>();
    VtArray<T> dst;
    dst.reserve(take.size() * stride);
    for (const int index : take) {
        for (size_t k = 0; k < stride; ++k) {
            const size_t at = static_cast<size_t>(index) * stride + k;
            if (at < src.size()) dst.push_back(src[at]);
        }
    }
    *out = VtValue(dst);
    return true;
}

// The elements `take` names, whatever array type the value holds.
bool Take(const VtValue& in, const std::vector<int>& take, size_t stride, VtValue* out) {
    return TakeAs<int>(in, take, stride, out) || TakeAs<float>(in, take, stride, out) ||
           TakeAs<GfVec3f>(in, take, stride, out) || TakeAs<GfVec2f>(in, take, stride, out) ||
           TakeAs<GfVec4f>(in, take, stride, out) || TakeAs<double>(in, take, stride, out) ||
           TakeAs<GfVec2d>(in, take, stride, out) || TakeAs<GfVec3d>(in, take, stride, out) ||
           TakeAs<GfVec4d>(in, take, stride, out) || TakeAs<GfHalf>(in, take, stride, out) ||
           TakeAs<GfVec2h>(in, take, stride, out) || TakeAs<GfVec3h>(in, take, stride, out) ||
           TakeAs<GfVec4h>(in, take, stride, out) || TakeAs<GfQuatf>(in, take, stride, out) ||
           TakeAs<bool>(in, take, stride, out) || TakeAs<unsigned char>(in, take, stride, out) ||
           TakeAs<int64_t>(in, take, stride, out) || TakeAs<unsigned int>(in, take, stride, out) ||
           TakeAs<std::string>(in, take, stride, out) || TakeAs<TfToken>(in, take, stride, out);
}

struct Part {
    std::string name;
    std::vector<int> faces;
    std::vector<std::pair<TfToken, SdfPathVector>> bindings;  // empty: inherits the mesh's own
    // derived
    std::vector<int> corners;   // indices into faceVertexIndices
    std::vector<int> vertices;  // the points this part uses, in order
    std::vector<int> vertexRemap;  // old point index -> new, -1 = unused
};

enum class Domain { Whole, Faces, Corners, Vertices };

Domain DomainOf(const TfToken& interpolation) {
    if (interpolation == UsdGeomTokens->uniform) return Domain::Faces;
    if (interpolation == UsdGeomTokens->faceVarying) return Domain::Corners;
    if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) return Domain::Vertices;
    return Domain::Whole;
}

// True when the mesh was split. `problem` says why not, when it should have been.
bool SplitOne(const UsdStageRefPtr& flat, UsdPrim meshPrim, size_t* partsMade, std::string* problem) {
    const UsdGeomMesh mesh(meshPrim);
    std::vector<UsdGeomSubset> subsets;
    for (const UsdGeomSubset& subset : UsdGeomSubset::GetAllGeomSubsets(mesh)) {
        TfToken element;
        subset.GetElementTypeAttr().Get(&element);
        if (element != UsdGeomTokens->face) continue;
        bool bound = false;
        for (const UsdRelationship& rel : subset.GetPrim().GetRelationships()) {
            SdfPathVector targets;
            if (rel.GetName().GetString().rfind("material:binding", 0) == 0 && rel.GetTargets(&targets) &&
                !targets.empty()) {
                bound = true;
            }
        }
        if (bound) subsets.push_back(subset);
    }
    if (subsets.empty()) return false;

    const UsdAttribute countsAttr = mesh.GetFaceVertexCountsAttr();
    const UsdAttribute indicesAttr = mesh.GetFaceVertexIndicesAttr();
    if (countsAttr.GetNumTimeSamples() > 0 || indicesAttr.GetNumTimeSamples() > 0) {
        *problem = "its topology is animated";
        return false;
    }
    VtIntArray counts, indices;
    if (!countsAttr.Get(&counts) || !indicesAttr.Get(&indices) || counts.empty()) return false;
    std::vector<int> firstCorner(counts.size() + 1, 0);
    for (size_t f = 0; f < counts.size(); ++f) firstCorner[f + 1] = firstCorner[f] + counts[f];
    if (static_cast<size_t>(firstCorner.back()) != indices.size()) {
        *problem = "its face counts and indices do not agree";
        return false;
    }
    int pointCount = 0;
    for (const int v : indices) pointCount = std::max(pointCount, v + 1);

    // Subdivision creases and corners index points; keeping every point in
    // every part keeps them valid. Without them the parts are compacted.
    const bool compact = !mesh.GetCreaseIndicesAttr().HasAuthoredValue() &&
                         !mesh.GetCornerIndicesAttr().HasAuthoredValue();

    std::vector<Part> parts;
    std::vector<char> taken(counts.size(), 0);
    for (const UsdGeomSubset& subset : subsets) {
        Part part;
        part.name = subset.GetPrim().GetName().GetString();
        VtIntArray faces;
        subset.GetIndicesAttr().Get(&faces);
        for (const int f : faces) {
            if (f < 0 || static_cast<size_t>(f) >= counts.size()) continue;
            part.faces.push_back(f);
            taken[f] = 1;
        }
        for (const UsdRelationship& rel : subset.GetPrim().GetRelationships()) {
            SdfPathVector targets;
            if (rel.GetName().GetString().rfind("material:binding", 0) == 0 && rel.GetTargets(&targets)) {
                part.bindings.emplace_back(rel.GetName(), targets);
            }
        }
        if (!part.faces.empty()) parts.push_back(part);
    }
    Part rest;
    rest.name = meshPrim.GetName().GetString() + "_rest";
    for (size_t f = 0; f < counts.size(); ++f) {
        if (!taken[f]) rest.faces.push_back(static_cast<int>(f));
    }
    if (!rest.faces.empty()) parts.push_back(rest);
    if (parts.empty()) return false;

    for (Part& part : parts) {
        part.vertexRemap.assign(pointCount, -1);
        for (const int f : part.faces) {
            for (int c = firstCorner[f]; c < firstCorner[f + 1]; ++c) {
                part.corners.push_back(c);
                const int v = indices[c];
                if (v >= 0 && part.vertexRemap[v] < 0) {
                    part.vertexRemap[v] = static_cast<int>(part.vertices.size());
                    part.vertices.push_back(v);
                }
            }
        }
        if (!compact) {
            part.vertices.resize(pointCount);
            std::iota(part.vertices.begin(), part.vertices.end(), 0);
            std::iota(part.vertexRemap.begin(), part.vertexRemap.end(), 0);
        }
    }

    // What every authored attribute of the mesh is indexed by.
    struct Source {
        UsdAttribute attr;
        Domain domain;
        size_t stride;
    };
    std::vector<Source> sources;
    std::vector<TfToken> geometryAttrs;
    for (const UsdAttribute& attr : meshPrim.GetAuthoredAttributes()) {
        const std::string name = attr.GetName().GetString();
        if (name.rfind("xformOp", 0) == 0 || name == "visibility" || name == "purpose") continue;  // inherited
        geometryAttrs.push_back(attr.GetName());
        if (name == "holeIndices") continue;  // face numbers that no longer mean anything
        Source source{attr, Domain::Whole, 1};
        if (name == "faceVertexCounts") {
            source.domain = Domain::Faces;
        } else if (name == "faceVertexIndices") {
            source.domain = Domain::Corners;
        } else if (name == "points" || name == "velocities" || name == "accelerations") {
            source.domain = Domain::Vertices;
        } else if (name == "normals" || name.rfind("primvars:", 0) == 0) {
            UsdAttribute owner = attr;  // "primvars:st:indices" is indexed like "primvars:st"
            const bool isIndices = name.size() > 8 && name.compare(name.size() - 8, 8, ":indices") == 0;
            if (isIndices) owner = meshPrim.GetAttribute(TfToken(name.substr(0, name.size() - 8)));
            TfToken interpolation = UsdGeomTokens->constant;
            if (name == "normals") interpolation = mesh.GetNormalsInterpolation();
            if (owner) owner.GetMetadata(UsdGeomTokens->interpolation, &interpolation);
            const bool indexed = !isIndices && meshPrim.GetAttribute(TfToken(name + ":indices")).HasAuthoredValue();
            if (!indexed) {  // an indexed primvar keeps its values; its indices are what gets cut
                source.domain = DomainOf(interpolation);
                int elementSize = 1;
                if (!isIndices && attr.GetMetadata(UsdGeomTokens->elementSize, &elementSize) && elementSize > 1) {
                    source.stride = static_cast<size_t>(elementSize);
                }
            }
        }
        sources.push_back(source);
    }

    // The subsets go, meshes of the same names take their place.
    for (const UsdGeomSubset& subset : subsets) flat->RemovePrim(subset.GetPrim().GetPath());
    const SdfLayerHandle layer = flat->GetRootLayer();
    for (const Part& part : parts) {
        const SdfPath path = meshPrim.GetPath().AppendChild(TfToken(part.name));
        UsdPrim child = UsdGeomMesh::Define(flat, path).GetPrim();
        for (const Source& source : sources) {
            const UsdAttribute& from = source.attr;
            UsdAttribute to = child.CreateAttribute(from.GetName(), from.GetTypeName(), from.IsCustom(),
                                                    from.GetVariability());
            for (const TfToken& key : {UsdGeomTokens->interpolation, UsdGeomTokens->elementSize, TfToken("colorSpace")}) {
                VtValue meta;
                if (from.HasAuthoredMetadata(key) && from.GetMetadata(key, &meta)) to.SetMetadata(key, meta);
            }
            const std::vector<int>* take = source.domain == Domain::Faces      ? &part.faces
                                           : source.domain == Domain::Corners  ? &part.corners
                                           : source.domain == Domain::Vertices ? &part.vertices
                                                                               : nullptr;
            const auto convert = [&](const VtValue& value) {
                VtValue out = value;
                if (take && !Take(value, *take, source.stride, &out)) return value;
                if (from.GetName() == "faceVertexIndices" && out.IsHolding<VtIntArray>()) {
                    VtIntArray remapped = out.UncheckedGet<VtIntArray>();
                    for (int& v : remapped) v = (v >= 0 && v < pointCount) ? part.vertexRemap[v] : 0;
                    out = VtValue(remapped);
                }
                return out;
            };
            const SdfAttributeSpecHandle spec = layer->GetAttributeAtPath(from.GetPath());
            VtValue value;
            if (spec && spec->HasDefaultValue() && from.Get(&value, UsdTimeCode::Default())) to.Set(convert(value));
            std::vector<double> times;
            from.GetTimeSamples(&times);
            for (const double time : times) {
                if (from.Get(&value, time)) to.Set(convert(value), time);
            }
        }
        if (!part.bindings.empty()) {
            UsdShadeMaterialBindingAPI::Apply(child);
            for (const auto& binding : part.bindings) {
                child.CreateRelationship(binding.first, /*custom=*/false).SetTargets(binding.second);
            }
        }
    }

    // What was the mesh is the group now: its name, transform, visibility,
    // purpose and its own material binding (the "rest" part inherits it).
    for (const TfToken& name : geometryAttrs) meshPrim.RemoveProperty(name);
    meshPrim.SetTypeName(TfToken("Xform"));
    *partsMade += parts.size();
    return true;
}

// ------------------------------------------------------- implicit shapes

template <typename T>
T ValueOr(const UsdPrim& prim, const char* name, T fallback) {
    T value = fallback;
    const UsdAttribute attr = prim.GetAttribute(TfToken(name));
    if (attr) attr.Get(&value, UsdTimeCode::EarliestTime());
    return value;
}

// The generators build along Z; this turns Z into the shape's own axis.
GfMatrix4d FrameFor(const TfToken& axis) {
    if (axis == UsdGeomTokens->x) return GfMatrix4d(0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1);
    if (axis == UsdGeomTokens->y) return GfMatrix4d(0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    return GfMatrix4d(1.0);
}

bool ShapeToMesh(UsdPrim prim) {
    constexpr size_t kRadial = 32, kAxial = 16, kCapAxial = 8;
    const std::string type = prim.GetTypeName().GetString();
    const GfMatrix4d frame = FrameFor(ValueOr<TfToken>(prim, "axis", UsdGeomTokens->z));
    VtVec3fArray points;
    PxOsdMeshTopology topology;
    if (type == "Sphere") {
        points.resize(GeomUtilSphereMeshGenerator::ComputeNumPoints(kRadial, kAxial));
        GeomUtilSphereMeshGenerator::GeneratePoints(points.begin(), kRadial, kAxial,
                                                    static_cast<float>(ValueOr<double>(prim, "radius", 1.0)));
        topology = GeomUtilSphereMeshGenerator::GenerateTopology(kRadial, kAxial);
    } else if (type == "Cube") {
        const float size = static_cast<float>(ValueOr<double>(prim, "size", 2.0));
        points.resize(GeomUtilCuboidMeshGenerator::ComputeNumPoints());
        GeomUtilCuboidMeshGenerator::GeneratePoints(points.begin(), size, size, size);
        topology = GeomUtilCuboidMeshGenerator::GenerateTopology();
    } else if (type == "Cylinder") {
        points.resize(GeomUtilCylinderMeshGenerator::ComputeNumPoints(kRadial));
        GeomUtilCylinderMeshGenerator::GeneratePoints(points.begin(), kRadial,
                                                      static_cast<float>(ValueOr<double>(prim, "radius", 1.0)),
                                                      static_cast<float>(ValueOr<double>(prim, "height", 2.0)), &frame);
        topology = GeomUtilCylinderMeshGenerator::GenerateTopology(kRadial);
    } else if (type == "Cone") {
        points.resize(GeomUtilConeMeshGenerator::ComputeNumPoints(kRadial));
        GeomUtilConeMeshGenerator::GeneratePoints(points.begin(), kRadial,
                                                  static_cast<float>(ValueOr<double>(prim, "radius", 1.0)),
                                                  static_cast<float>(ValueOr<double>(prim, "height", 2.0)), &frame);
        topology = GeomUtilConeMeshGenerator::GenerateTopology(kRadial);
    } else if (type == "Capsule") {
        points.resize(GeomUtilCapsuleMeshGenerator::ComputeNumPoints(kRadial, kCapAxial));
        GeomUtilCapsuleMeshGenerator::GeneratePoints(points.begin(), kRadial, kCapAxial,
                                                     static_cast<float>(ValueOr<double>(prim, "radius", 0.5)),
                                                     static_cast<float>(ValueOr<double>(prim, "height", 1.0)), &frame);
        topology = GeomUtilCapsuleMeshGenerator::GenerateTopology(kRadial, kCapAxial);
    } else {
        return false;
    }

    for (const char* name : {"radius", "height", "size", "axis", "extent"}) prim.RemoveProperty(TfToken(name));
    prim.SetTypeName(TfToken("Mesh"));
    const UsdGeomMesh mesh(prim);
    mesh.CreatePointsAttr().Set(points);
    mesh.CreateFaceVertexCountsAttr().Set(topology.GetFaceVertexCounts());
    mesh.CreateFaceVertexIndicesAttr().Set(topology.GetFaceVertexIndices());
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreateOrientationAttr().Set(topology.GetOrientation());
    VtVec3fArray extent;
    if (UsdGeomPointBased::ComputeExtent(points, &extent)) mesh.CreateExtentAttr().Set(extent);
    return true;
}

}  // namespace

void MakeGeometryNukeReadable(Report& rep, const UsdStageRefPtr& flat) {
    std::vector<UsdPrim> meshes, shapes;
    std::vector<std::string> curves, volumes;
    for (const UsdPrim& prim : flat->Traverse()) {
        const TfToken type = prim.GetTypeName();
        if (prim.IsA<UsdGeomMesh>()) {
            meshes.push_back(prim);
        } else if (type == "Sphere" || type == "Cube" || type == "Cylinder" || type == "Cone" || type == "Capsule") {
            shapes.push_back(prim);
        } else if (prim.IsA<UsdGeomCurves>()) {
            curves.push_back(prim.GetPath().GetString());
        } else if (type == "Volume") {
            volumes.push_back(prim.GetPath().GetString());
        }
    }

    std::vector<std::string> split, notSplit;
    size_t parts = 0;
    for (const UsdPrim& prim : meshes) {
        std::string problem;
        const std::string path = prim.GetPath().GetString();
        if (SplitOne(flat, prim, &parts, &problem)) {
            split.push_back(path);
        } else if (!problem.empty()) {
            notSplit.push_back(path + " (" + problem + ")");
        }
    }
    if (!split.empty()) {
        rep.Info("nuke", std::to_string(split.size()) + " mesh(es) with per-face materials split into " +
                             std::to_string(parts) +
                             " meshes, one per material, inside a group of the original name - Nuke ignores "
                             "materials bound to face sets: " + NameList(split));
    }
    if (!notSplit.empty()) {
        rep.Warn("nuke", "per-face materials Nuke will not show (the mesh could not be split): " + NameList(notSplit));
    }

    std::vector<std::string> meshed;
    for (UsdPrim prim : shapes) {
        const std::string type = prim.GetTypeName().GetString();
        if (ShapeToMesh(prim)) meshed.push_back(prim.GetPath().GetString() + " (" + type + ")");
    }
    if (!meshed.empty()) {
        rep.Info("nuke", std::to_string(meshed.size()) +
                             " implicit shape(s) turned into meshes - Nuke does not draw them: " + NameList(meshed));
    }
    if (!volumes.empty()) {
        rep.Warn("nuke", std::to_string(volumes.size()) +
                             " volume(s) (smoke, clouds) are in the file but Nuke's 3D render does not draw them: " +
                             NameList(volumes));
    }
    if (!curves.empty()) {
        rep.Warn("nuke", std::to_string(curves.size()) +
                             " curve object(s) are in the file but Nuke does not draw curves: " + NameList(curves));
    }
}

}  // namespace detail
}  // namespace usdprep
