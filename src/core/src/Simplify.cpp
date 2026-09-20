// Simplify: fewer polygons, same look - as far as a decimator can manage.
//
// meshoptimizer collapses edges onto existing vertices, so a decimated
// mesh is a subset of the original vertices with a new triangle list.
// That is what makes animation survive it: every time sample of points
// (and of every per-vertex attribute) is remapped through the same
// subset. UV seams and hard normals are handled by splitting vertices
// beforehand - one render vertex per unique (point, attribute values)
// combination - and letting the simplifier weld positions back together
// for its own adjacency, which is how it keeps seams where they are.
//
// The output is a triangle mesh with every carried attribute
// vertex-interpolated (points duplicated along seams). Face-varying data
// does not exist any more; neither do creases, corners, holes or a
// subdivision scheme - a decimated cage is not a cage.
#include "Shared.h"

#include <cstring>
#include <map>
#include <unordered_map>

#include <meshoptimizer.h>

#include <pxr/base/gf/range3f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>

namespace usdprep {
namespace detail {

namespace {

// A per-corner attribute the decimation carries along: a primvar or the
// mesh's normals, flattened to floats.
struct CornerStream {
    std::string name;
    UsdAttribute attr;          // where it is written back
    bool isPrimvar = false;
    TfToken interpolation;      // as authored
    int components = 0;
    float weight = 0.0f;        // how much the simplifier protects it
    std::vector<float> values;  // components * cornerCount, at the reference time
    std::vector<double> times;  // extra time samples to carry (empty = static)
};

// Float-family value arrays only; anything else cannot ride along.
bool ToFloats(const VtValue& value, int* components, std::vector<float>* out) {
    if (value.IsHolding<VtArray<float>>()) {
        const auto& a = value.UncheckedGet<VtArray<float>>();
        *components = 1;
        out->assign(a.begin(), a.end());
        return true;
    }
    if (value.IsHolding<VtArray<GfVec2f>>()) {
        const auto& a = value.UncheckedGet<VtArray<GfVec2f>>();
        *components = 2;
        out->resize(a.size() * 2);
        for (size_t i = 0; i < a.size(); ++i) { (*out)[i * 2] = a[i][0]; (*out)[i * 2 + 1] = a[i][1]; }
        return true;
    }
    if (value.IsHolding<VtArray<GfVec3f>>()) {
        const auto& a = value.UncheckedGet<VtArray<GfVec3f>>();
        *components = 3;
        out->resize(a.size() * 3);
        for (size_t i = 0; i < a.size(); ++i) { std::memcpy(&(*out)[i * 3], a[i].data(), sizeof(float) * 3); }
        return true;
    }
    if (value.IsHolding<VtArray<GfVec4f>>()) {
        const auto& a = value.UncheckedGet<VtArray<GfVec4f>>();
        *components = 4;
        out->resize(a.size() * 4);
        for (size_t i = 0; i < a.size(); ++i) { std::memcpy(&(*out)[i * 4], a[i].data(), sizeof(float) * 4); }
        return true;
    }
    return false;
}

// Back into the array type the attribute was authored with.
VtValue FromFloats(const VtValue& like, int components, const std::vector<float>& values) {
    const size_t n = values.size() / components;
    if (like.IsHolding<VtArray<float>>()) {
        return VtValue(VtArray<float>(values.begin(), values.end()));
    }
    if (like.IsHolding<VtArray<GfVec2f>>()) {
        VtArray<GfVec2f> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = GfVec2f(values[i * 2], values[i * 2 + 1]);
        return VtValue(a);
    }
    if (like.IsHolding<VtArray<GfVec3f>>()) {
        VtArray<GfVec3f> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = GfVec3f(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
        return VtValue(a);
    }
    VtArray<GfVec4f> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = GfVec4f(values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3]);
    return VtValue(a);
}

// Values per element -> values per corner, following the interpolation.
bool ExpandToCorners(const std::vector<float>& perElement, int components, const TfToken& interpolation,
                     const VtArray<int>& faceVertexIndices, size_t pointCount, std::vector<float>* perCorner) {
    const size_t corners = faceVertexIndices.size();
    if (interpolation == UsdGeomTokens->faceVarying) {
        if (perElement.size() != corners * components) return false;
        *perCorner = perElement;
        return true;
    }
    if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) {
        if (perElement.size() != pointCount * components) return false;
        perCorner->resize(corners * components);
        for (size_t c = 0; c < corners; ++c) {
            const int p = faceVertexIndices[c];
            if (p < 0 || static_cast<size_t>(p) >= pointCount) return false;
            std::memcpy(&(*perCorner)[c * components], &perElement[static_cast<size_t>(p) * components],
                        sizeof(float) * components);
        }
        return true;
    }
    return false;
}

struct MeshResult {
    bool done = false;
    size_t facesBefore = 0;
    size_t facesAfter = 0;
    std::string skipped;                 // why the mesh was left alone
    std::vector<std::string> dropped;    // attributes that could not ride along
};

MeshResult SimplifyOne(const UsdStageRefPtr& stage, UsdPrim prim, double ratio, size_t minFaces) {
    MeshResult result;
    UsdGeomMesh mesh(prim);

    // ----- what the mesh is made of -----------------------------------------
    VtArray<int> faceVertexCounts;
    VtArray<int> faceVertexIndices;
    mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts);
    mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices);
    result.facesBefore = faceVertexCounts.size();
    if (faceVertexCounts.size() < minFaces) {
        result.skipped = "small";
        return result;
    }
    if (mesh.GetFaceVertexIndicesAttr().GetNumTimeSamples() > 0 ||
        mesh.GetFaceVertexCountsAttr().GetNumTimeSamples() > 0) {
        result.skipped = "animated topology";
        return result;
    }
    if (!UsdGeomSubset::GetAllGeomSubsets(mesh).empty()) {
        result.skipped = "per-face material subsets";
        return result;
    }

    const UsdAttribute pointsAttr = mesh.GetPointsAttr();
    std::vector<double> pointTimes;
    pointsAttr.GetTimeSamples(&pointTimes);
    const UsdTimeCode referenceTime = pointTimes.empty() ? UsdTimeCode::Default() : UsdTimeCode(pointTimes.front());
    VtArray<GfVec3f> points;
    if (!pointsAttr.Get(&points, referenceTime) || points.empty()) {
        result.skipped = "no points";
        return result;
    }
    const size_t pointCount = points.size();
    const size_t cornerCount = faceVertexIndices.size();
    for (const int index : faceVertexIndices) {
        if (index < 0 || static_cast<size_t>(index) >= pointCount) {
            result.skipped = "broken indices";
            return result;
        }
    }

    // ----- the attributes that ride along -------------------------------------
    std::vector<CornerStream> streams;
    std::vector<std::string> removedProperties;  // uniform primvars, indices, ...

    const auto addStream = [&](const std::string& name, const UsdAttribute& attr, bool isPrimvar,
                               const TfToken& interpolation, const VtValue& flattened, float weight) {
        if (interpolation == UsdGeomTokens->constant) return;  // stays as it is
        if (interpolation == UsdGeomTokens->uniform) {
            // per face: the faces are about to change
            result.dropped.push_back(name);
            removedProperties.push_back(attr.GetName().GetString());
            return;
        }
        CornerStream stream;
        stream.name = name;
        stream.attr = attr;
        stream.isPrimvar = isPrimvar;
        stream.interpolation = interpolation;
        stream.weight = weight;
        std::vector<float> perElement;
        if (!ToFloats(flattened, &stream.components, &perElement)) {
            result.dropped.push_back(name);
            removedProperties.push_back(attr.GetName().GetString());
            return;
        }
        // one value under a per-vertex label is a constant in disguise:
        // it stays as it is
        if (perElement.size() == static_cast<size_t>(stream.components)) return;
        if (!ExpandToCorners(perElement, stream.components, interpolation, faceVertexIndices, pointCount,
                             &stream.values)) {
            result.dropped.push_back(name);
            removedProperties.push_back(attr.GetName().GetString());
            return;
        }
        attr.GetTimeSamples(&stream.times);
        streams.push_back(std::move(stream));
    };

    UsdGeomPrimvarsAPI primvarsAPI(prim);
    for (const UsdGeomPrimvar& primvar : primvarsAPI.GetPrimvarsWithValues()) {
        const std::string name = primvar.GetPrimvarName().GetString();
        VtValue flattened;
        primvar.ComputeFlattened(&flattened, referenceTime);
        const bool isUv = primvar.GetTypeName() == SdfValueTypeNames->TexCoord2fArray ||
                          primvar.GetTypeName() == SdfValueTypeNames->TexCoord3fArray ||
                          name == "st" || name.rfind("st_", 0) == 0 || name.rfind("uv", 0) == 0;
        addStream("primvars:" + name, primvar.GetAttr(), true, primvar.GetInterpolation(), flattened,
                  isUv ? 0.5f : 0.15f);
        if (primvar.IsIndexed()) removedProperties.push_back(primvar.GetIndicesAttr().GetName().GetString());
    }
    if (mesh.GetNormalsAttr().HasAuthoredValue()) {
        VtValue normals;
        mesh.GetNormalsAttr().Get(&normals, referenceTime);
        addStream("normals", mesh.GetNormalsAttr(), false, mesh.GetNormalsInterpolation(), normals, 0.3f);
    }

    // ----- render vertices: one per unique (point, attribute values) -------
    // meshoptimizer takes at most 32 attribute floats per vertex; the
    // streams that matter most come first.
    std::stable_sort(streams.begin(), streams.end(),
                     [](const CornerStream& a, const CornerStream& b) { return a.weight > b.weight; });
    int attributeFloats = 0;
    for (const CornerStream& s : streams) attributeFloats += s.components;
    std::vector<size_t> attributeStreams;  // indices of streams that fit the simplifier's metric
    {
        int used = 0;
        for (size_t i = 0; i < streams.size(); ++i) {
            if (used + streams[i].components > 32) continue;
            attributeStreams.push_back(i);
            used += streams[i].components;
        }
        attributeFloats = used;
    }

    struct VertexKey {
        int point;
        std::vector<float> attributes;
        bool operator<(const VertexKey& o) const {
            if (point != o.point) return point < o.point;
            return attributes < o.attributes;
        }
    };
    std::map<VertexKey, unsigned int> vertexIds;
    std::vector<unsigned int> cornerToVertex(cornerCount);
    std::vector<int> vertexPoint;       // render vertex -> point
    std::vector<size_t> vertexCorner;   // render vertex -> a corner it came from
    for (size_t c = 0; c < cornerCount; ++c) {
        VertexKey key;
        key.point = faceVertexIndices[c];
        for (const CornerStream& s : streams) {
            key.attributes.insert(key.attributes.end(), s.values.begin() + c * s.components,
                                  s.values.begin() + (c + 1) * s.components);
        }
        const auto found = vertexIds.find(key);
        if (found != vertexIds.end()) {
            cornerToVertex[c] = found->second;
        } else {
            const unsigned int id = static_cast<unsigned int>(vertexPoint.size());
            vertexIds.emplace(std::move(key), id);
            cornerToVertex[c] = id;
            vertexPoint.push_back(faceVertexIndices[c]);
            vertexCorner.push_back(c);
        }
    }
    const size_t vertexCount = vertexPoint.size();

    std::vector<float> positions(vertexCount * 3);
    for (size_t v = 0; v < vertexCount; ++v) {
        std::memcpy(&positions[v * 3], points[vertexPoint[v]].data(), sizeof(float) * 3);
    }
    std::vector<float> attributes(vertexCount * std::max(attributeFloats, 1));
    std::vector<float> weights;
    for (const size_t si : attributeStreams) {
        for (int k = 0; k < streams[si].components; ++k) weights.push_back(streams[si].weight);
    }
    if (attributeFloats > 0) {
        for (size_t v = 0; v < vertexCount; ++v) {
            float* dst = &attributes[v * attributeFloats];
            for (const size_t si : attributeStreams) {
                const CornerStream& s = streams[si];
                std::memcpy(dst, &s.values[vertexCorner[v] * s.components], sizeof(float) * s.components);
                dst += s.components;
            }
        }
    }

    // ----- triangles (fans), holes left out ---------------------------------
    std::set<int> holes;
    {
        VtArray<int> holeIndices;
        if (mesh.GetHoleIndicesAttr().Get(&holeIndices)) holes.insert(holeIndices.begin(), holeIndices.end());
    }
    std::vector<unsigned int> indices;
    indices.reserve(cornerCount * 3);
    size_t corner = 0;
    for (size_t f = 0; f < faceVertexCounts.size(); ++f) {
        const int n = faceVertexCounts[f];
        if (n >= 3 && !holes.count(static_cast<int>(f))) {
            for (int k = 1; k + 1 < n; ++k) {
                indices.push_back(cornerToVertex[corner]);
                indices.push_back(cornerToVertex[corner + k]);
                indices.push_back(cornerToVertex[corner + k + 1]);
            }
        }
        corner += std::max(n, 0);
    }
    if (indices.size() < 3) {
        result.skipped = "no triangles";
        return result;
    }
    // the ratio and the report are about triangles: that is what the
    // decimator counts, and what the output is made of
    result.facesBefore = indices.size() / 3;

    // ----- decimate -------------------------------------------------------------
    size_t target = static_cast<size_t>(indices.size() * ratio);
    target -= target % 3;
    target = std::max<size_t>(target, 3);
    std::vector<unsigned int> simplified(indices.size());
    float resultError = 0.0f;
    const size_t newIndexCount = meshopt_simplifyWithAttributes(
        simplified.data(), indices.data(), indices.size(), positions.data(), vertexCount, sizeof(float) * 3,
        attributeFloats > 0 ? attributes.data() : nullptr, sizeof(float) * attributeFloats,
        attributeFloats > 0 ? weights.data() : nullptr, static_cast<size_t>(attributeFloats), nullptr, target,
        // The ratio is the contract: whoever asked for a quarter gets a
        // quarter. No error ceiling, the attribute-aware metric still
        // picks the least damaging collapses first.
        /*target_error=*/1.0f, 0, &resultError);
    simplified.resize(newIndexCount);
    if (newIndexCount >= indices.size()) {
        result.skipped = "nothing to gain";
        return result;
    }

    // ----- compact: only the vertices the triangles still use -------------------
    std::vector<unsigned int> newIdOf(vertexCount, ~0u);
    std::vector<unsigned int> keptVertices;
    for (unsigned int& index : simplified) {
        if (newIdOf[index] == ~0u) {
            newIdOf[index] = static_cast<unsigned int>(keptVertices.size());
            keptVertices.push_back(index);
        }
        index = newIdOf[index];
    }

    // ----- write back --------------------------------------------------------------
    const auto pointsAt = [&](UsdTimeCode time, VtArray<GfVec3f>* out) {
        VtArray<GfVec3f> source;
        if (!pointsAttr.Get(&source, time) || source.size() != pointCount) return false;
        out->resize(keptVertices.size());
        for (size_t v = 0; v < keptVertices.size(); ++v) (*out)[v] = source[vertexPoint[keptVertices[v]]];
        return true;
    };
    const auto extentOf = [](const VtArray<GfVec3f>& pts) {
        GfRange3f range;
        for (const GfVec3f& p : pts) range.UnionWith(p);
        VtArray<GfVec3f> extent(2);
        extent[0] = range.GetMin();
        extent[1] = range.GetMax();
        return extent;
    };

    // Everything is read before anything is cleared: Clear() takes the
    // source values with it.
    const UsdAttribute extentAttr = mesh.GetExtentAttr();
    std::vector<std::pair<UsdTimeCode, VtArray<GfVec3f>>> newPoints;
    if (pointTimes.empty()) {
        VtArray<GfVec3f> pts;
        if (pointsAt(UsdTimeCode::Default(), &pts)) newPoints.emplace_back(UsdTimeCode::Default(), pts);
    }
    for (const double t : pointTimes) {
        VtArray<GfVec3f> pts;
        if (pointsAt(UsdTimeCode(t), &pts)) newPoints.emplace_back(UsdTimeCode(t), pts);
    }
    pointsAttr.Clear();
    extentAttr.Clear();
    for (const auto& sample : newPoints) {
        pointsAttr.Set(sample.second, sample.first);
        extentAttr.Set(extentOf(sample.second), sample.first);
    }

    VtArray<int> newCounts(simplified.size() / 3, 3);
    VtArray<int> newIndices(simplified.begin(), simplified.end());
    mesh.GetFaceVertexCountsAttr().Set(newCounts);
    mesh.GetFaceVertexIndicesAttr().Set(newIndices);
    result.facesAfter = newCounts.size();

    for (const CornerStream& s : streams) {
        // reference-time values come from the split vertices themselves
        const auto valuesAt = [&](UsdTimeCode time, std::vector<float>* out) {
            std::vector<float> perCorner;
            if (time == referenceTime) {
                perCorner = s.values;
            } else {
                VtValue value;
                if (s.isPrimvar) {
                    UsdGeomPrimvar(s.attr).ComputeFlattened(&value, time);
                } else {
                    s.attr.Get(&value, time);
                }
                std::vector<float> perElement;
                int components = 0;
                if (!ToFloats(value, &components, &perElement) || components != s.components ||
                    !ExpandToCorners(perElement, components, s.interpolation, faceVertexIndices, pointCount,
                                     &perCorner)) {
                    return false;
                }
            }
            out->resize(keptVertices.size() * s.components);
            for (size_t v = 0; v < keptVertices.size(); ++v) {
                std::memcpy(&(*out)[v * s.components], &perCorner[vertexCorner[keptVertices[v]] * s.components],
                            sizeof(float) * s.components);
            }
            return true;
        };
        VtValue like;
        if (s.isPrimvar) {
            UsdGeomPrimvar(s.attr).ComputeFlattened(&like, referenceTime);
        } else {
            s.attr.Get(&like, referenceTime);
        }
        // read everything first, then clear, then write (see the points)
        std::vector<std::pair<UsdTimeCode, VtValue>> samples;
        std::vector<float> values;
        if (s.times.empty() && valuesAt(UsdTimeCode::Default(), &values)) {
            samples.emplace_back(UsdTimeCode::Default(), FromFloats(like, s.components, values));
        }
        for (const double t : s.times) {
            if (valuesAt(UsdTimeCode(t), &values)) {
                samples.emplace_back(UsdTimeCode(t), FromFloats(like, s.components, values));
            }
        }
        s.attr.Clear();
        for (const auto& sample : samples) s.attr.Set(sample.second, sample.first);
        if (s.isPrimvar) {
            UsdGeomPrimvar(s.attr).SetInterpolation(UsdGeomTokens->vertex);
        } else {
            mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
        }
    }

    // What a decimated triangle mesh cannot carry.
    static const char* kSubdivProperties[] = {"creaseIndices",  "creaseLengths",     "creaseSharpnesses",
                                              "cornerIndices",  "cornerSharpnesses", "holeIndices"};
    for (const char* name : kSubdivProperties) {
        if (prim.HasProperty(TfToken(name))) prim.RemoveProperty(TfToken(name));
    }
    for (const std::string& name : removedProperties) {
        if (prim.HasProperty(TfToken(name))) prim.RemoveProperty(TfToken(name));
    }
    mesh.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->none);

    result.done = true;
    return result;
}

}  // namespace

void SimplifyMeshes(Report& rep, const UsdStageRefPtr& flat, double ratio, size_t minFaces) {
    if (!(ratio > 0.0 && ratio < 1.0)) return;

    std::vector<UsdPrim> meshes;
    for (const UsdPrim& prim : UsdPrimRange(flat->GetPseudoRoot(), UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        if (!prim.IsInstanceProxy() && prim.IsA<UsdGeomMesh>()) meshes.push_back(prim);
    }

    size_t done = 0;
    size_t facesBefore = 0;
    size_t facesAfter = 0;
    std::map<std::string, size_t> skipped;
    std::set<std::string> dropped;
    for (const UsdPrim& prim : meshes) {
        const MeshResult r = SimplifyOne(flat, prim, ratio, minFaces);
        if (r.done) {
            ++done;
            facesBefore += r.facesBefore;
            facesAfter += r.facesAfter;
            dropped.insert(r.dropped.begin(), r.dropped.end());
        } else if (r.skipped != "small") {
            ++skipped[r.skipped];
        }
    }

    if (done > 0) {
        const int percent = facesBefore ? static_cast<int>(100.0 * facesAfter / facesBefore) : 100;
        rep.Info("simplify", std::to_string(done) + " mesh(es) reduced from " + std::to_string(facesBefore) +
                                 " to " + std::to_string(facesAfter) + " triangles (" + std::to_string(percent) +
                                 " %)");
    }
    for (const auto& entry : skipped) {
        rep.Warn("simplify", std::to_string(entry.second) + " mesh(es) left as they are: " + entry.first);
    }
    if (!dropped.empty()) {
        std::string names;
        size_t listed = 0;
        for (const std::string& name : dropped) {
            if (listed++ == 4) { names += ", ..."; break; }
            names += (names.empty() ? "" : ", ") + name;
        }
        rep.Warn("simplify", "attribute(s) a decimated mesh cannot carry were dropped: " + names);
    }
}

}  // namespace detail
}  // namespace usdprep
