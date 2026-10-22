// Alembic in: an .abc turned into USD.
//
// What USD's own Alembic plugin (usdAbc) makes of a file is the reference:
// the hierarchy as it is, a transform with a single shape below it merged
// into one prim named after the transform, every sample kept - transforms,
// point caches, changing topology, cameras - on the archive's own frames.
// Polygons are wound the other way round in Alembic; they are turned to
// USD's winding with everything that runs per corner. Cameras come in
// millimetres (Alembic stores centimetres), with the lens squeeze in the
// horizontal aperture. Alembic carries no materials.

#include "ImportShared.h"

#ifdef USDPREP_WITH_ALEMBIC

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <usdprep/Import.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdprep {
namespace importer {

namespace {

namespace Abc = Alembic::Abc;
namespace AbcA = Alembic::AbcCoreAbstract;
namespace AbcG = Alembic::AbcGeom;

// ------------------------------------------------------------------ the run

struct Run {
    UsdStageRefPtr stage;
    Report* rep = nullptr;
    Progress* progress = nullptr;
    double fps = 24.0;
    double firstFrame = std::numeric_limits<double>::infinity();
    double lastFrame = -std::numeric_limits<double>::infinity();
    size_t objectsTotal = 0;
    size_t objectsDone = 0;
    bool cancelled = false;
    // what came in, for the report
    size_t meshes = 0, animatedMeshes = 0, changingTopology = 0, subds = 0, cameras = 0, animatedCameras = 0;
    size_t points = 0, curves = 0, transforms = 0, animatedTransforms = 0, faceSets = 0;
    std::map<std::string, size_t> leftOut;  // kind -> how many
    std::set<std::string> colourSets;
    std::set<std::string> otherParams;
    std::set<std::string> paramsLeftOut;

    // Seconds to the frame number USD keeps: on the archive's own rate, a
    // hair of rounding noise taken off (1000.9999999 is frame 1001).
    UsdTimeCode Time(double seconds) {
        const double frame = std::round(seconds * fps * 10000.0) / 10000.0;
        firstFrame = std::min(firstFrame, frame);
        lastFrame = std::max(lastFrame, frame);
        return UsdTimeCode(frame);
    }

    bool Tick() {
        ++objectsDone;
        if (progress) {
            if (progress->cancel) cancelled = true;
            const float at = 0.05f + 0.85f * static_cast<float>(objectsDone) / static_cast<float>(std::max<size_t>(objectsTotal, 1));
            if (at > progress->fraction) progress->fraction = at;
        }
        return !cancelled;
    }
};

// The time of sample `i` of something with its own time sampling, or the
// default time when it does not change.
template <typename Sampled>
UsdTimeCode TimeOf(Run& run, const Sampled& sampled, size_t i, bool animated) {
    if (!animated) return UsdTimeCode::Default();
    return run.Time(sampled.getTimeSampling()->getSampleTime(static_cast<AbcA::index_t>(i)));
}

// ------------------------------------------------------------ the winding

// Alembic winds a polygon clockwise, USD counter-clockwise: every face's
// corners in reverse order. order[c] is the Alembic corner that becomes
// USD corner c.
std::vector<size_t> CornerOrder(const int32_t* counts, size_t faceCount) {
    std::vector<size_t> order;
    size_t start = 0;
    for (size_t f = 0; f < faceCount; ++f) {
        const size_t n = static_cast<size_t>(std::max(counts[f], 0));
        for (size_t k = 0; k < n; ++k) order.push_back(start + n - 1 - k);
        start += n;
    }
    return order;
}

template <typename T>
void Reorder(VtArray<T>* values, const std::vector<size_t>& order) {
    if (values->size() != order.size()) return;
    VtArray<T> out(values->size());
    for (size_t c = 0; c < order.size(); ++c) out[c] = (*values)[order[c]];
    values->swap(out);
}

template <typename Usd, typename Abc>
VtArray<Usd> ToVt(const Abc* data, size_t count) {
    static_assert(sizeof(Usd) == sizeof(Abc), "the same floats, laid out the same way");
    VtArray<Usd> out(count);
    if (count) std::memcpy(out.data(), data, count * sizeof(Usd));
    return out;
}

VtIntArray ToIntArray(const Abc::Int32ArraySamplePtr& sample) {
    if (!sample) return VtIntArray();
    return ToVt<int, int32_t>(sample->get(), sample->size());
}

TfToken InterpolationOf(AbcG::GeometryScope scope) {
    switch (scope) {
    case AbcG::kConstantScope: return UsdGeomTokens->constant;
    case AbcG::kUniformScope: return UsdGeomTokens->uniform;
    case AbcG::kVaryingScope: return UsdGeomTokens->varying;
    case AbcG::kVertexScope: return UsdGeomTokens->vertex;
    case AbcG::kFacevaryingScope: return UsdGeomTokens->faceVarying;
    default: return UsdGeomTokens->vertex;
    }
}

// The face counts of a mesh at each of its samples (only one when the
// topology does not change): what the per-corner data is turned with.
class Topology {
public:
    void Add(const VtIntArray& counts) {
        _orders.push_back(CornerOrder(counts.cdata(), counts.size()));
    }
    const std::vector<size_t>& At(size_t sample) const {
        static const std::vector<size_t> none;
        if (_orders.empty()) return none;
        return _orders[std::min(sample, _orders.size() - 1)];
    }

private:
    std::vector<std::vector<size_t>> _orders;
};

// ---------------------------------------------------------- geometry params

// One Alembic geometry parameter as a USD primvar, every sample of it,
// indexed when it is indexed, face-varying values turned with the faces.
template <typename Param, typename Usd>
bool PrimvarFrom(Run& run, Param param, UsdGeomPrimvarsAPI api, const TfToken& name, const SdfValueTypeName& type,
                 const Topology& topology) {
    if (!param.valid()) return false;
    const size_t samples = std::max<size_t>(param.getNumSamples(), 1);
    const bool animated = !param.isConstant();
    const TfToken interpolation = InterpolationOf(param.getScope());
    UsdGeomPrimvar primvar = api.CreatePrimvar(name, type, interpolation);
    for (size_t i = 0; i < samples; ++i) {
        const Abc::ISampleSelector at(static_cast<AbcA::index_t>(i));
        const UsdTimeCode time = TimeOf(run, param, i, animated);
        const std::vector<size_t>& order = topology.At(i);
        typename Param::Sample sample;
        if (param.isIndexed()) {
            param.getIndexed(sample, at);
            if (!sample.getVals()) continue;
            VtArray<Usd> values = ToVt<Usd>(sample.getVals()->get(), sample.getVals()->size());
            VtIntArray indices;
            if (sample.getIndices()) {
                indices = ToVt<int, uint32_t>(sample.getIndices()->get(), sample.getIndices()->size());
                if (interpolation == UsdGeomTokens->faceVarying) Reorder(&indices, order);
            }
            primvar.Set(values, time);
            primvar.SetIndices(indices, time);
        } else {
            param.getExpanded(sample, at);
            if (!sample.getVals()) continue;
            VtArray<Usd> values = ToVt<Usd>(sample.getVals()->get(), sample.getVals()->size());
            if (interpolation == UsdGeomTokens->faceVarying) Reorder(&values, order);
            primvar.Set(values, time);
        }
    }
    return true;
}

bool IsColourName(const std::string& name) {
    const std::string lower = Lower(name);
    return lower == "cd" || lower == "color" || lower == "colour" || lower == "rgb" || lower == "displaycolor" ||
           lower.rfind("colorset", 0) == 0 || lower.rfind("colourset", 0) == 0;
}

// The extra parameters a mesh carries: further UV sets, colours, any
// float data. Colours also become the display colour Nuke shows when a
// mesh has no material - Alembic has none.
void ArbitraryParams(Run& run, const Abc::ICompoundProperty& arb, UsdGeomPrimvarsAPI api, const Topology& topology,
                     bool* displayColourSet) {
    if (!arb.valid()) return;
    for (size_t i = 0; i < arb.getNumProperties(); ++i) {
        const AbcA::PropertyHeader& header = arb.getPropertyHeader(i);
        const std::string original = header.getName();
        const TfToken name(PrimNameFor(original, "param"));
        bool done = false;
        if (AbcG::IV2fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IV2fGeomParam, GfVec2f>(run, AbcG::IV2fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->TexCoord2fArray, topology);
        } else if (AbcG::IC3fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IC3fGeomParam, GfVec3f>(run, AbcG::IC3fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->Color3fArray, topology);
            if (done && !*displayColourSet && IsColourName(original)) {
                PrimvarFrom<AbcG::IC3fGeomParam, GfVec3f>(run, AbcG::IC3fGeomParam(arb, original), api,
                                                         TfToken("displayColor"),
                                                         SdfValueTypeNames->Color3fArray, topology);
                *displayColourSet = true;
                run.colourSets.insert(original);
            }
        } else if (AbcG::IC4fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IC4fGeomParam, GfVec4f>(run, AbcG::IC4fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->Color4fArray, topology);
            if (done && !*displayColourSet && IsColourName(original)) {
                // display colour is three channels: the colour set read again without its alpha
                UsdGeomPrimvar rgba = api.GetPrimvar(name);
                UsdGeomPrimvar rgb = api.CreatePrimvar(TfToken("displayColor"),
                                                       SdfValueTypeNames->Color3fArray, rgba.GetInterpolation());
                std::vector<double> times;
                rgba.GetAttr().GetTimeSamples(&times);
                if (times.empty()) times.push_back(std::numeric_limits<double>::quiet_NaN());
                for (const double t : times) {
                    const UsdTimeCode time = std::isnan(t) ? UsdTimeCode::Default() : UsdTimeCode(t);
                    VtVec4fArray values;
                    if (!rgba.Get(&values, time)) continue;
                    VtVec3fArray colours(values.size());
                    for (size_t v = 0; v < values.size(); ++v) colours[v] = GfVec3f(values[v][0], values[v][1], values[v][2]);
                    rgb.Set(colours, time);
                    VtIntArray indices;
                    if (rgba.GetIndices(&indices, time)) rgb.SetIndices(indices, time);
                }
                *displayColourSet = true;
                run.colourSets.insert(original);
            }
        } else if (AbcG::IN3fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IN3fGeomParam, GfVec3f>(run, AbcG::IN3fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->Normal3fArray, topology);
        } else if (AbcG::IV3fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IV3fGeomParam, GfVec3f>(run, AbcG::IV3fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->Vector3fArray, topology);
        } else if (AbcG::IP3fGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IP3fGeomParam, GfVec3f>(run, AbcG::IP3fGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->Point3fArray, topology);
        } else if (AbcG::IFloatGeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IFloatGeomParam, float>(run, AbcG::IFloatGeomParam(arb, original), api, name,
                                                            SdfValueTypeNames->FloatArray, topology);
        } else if (AbcG::IInt32GeomParam::matches(header)) {
            done = PrimvarFrom<AbcG::IInt32GeomParam, int>(run, AbcG::IInt32GeomParam(arb, original), api, name,
                                                          SdfValueTypeNames->IntArray, topology);
        }
        if (done) {
            run.otherParams.insert(original);
        } else {
            run.paramsLeftOut.insert(original);
        }
    }
}

// ------------------------------------------------------------------ shapes

void Visibility(Run& run, const Abc::IObject& object, UsdGeomImageable imageable) {
    AbcG::IVisibilityProperty visibility = AbcG::GetVisibilityProperty(const_cast<Abc::IObject&>(object));
    if (!visibility.valid()) return;
    const size_t samples = std::max<size_t>(visibility.getNumSamples(), 1);
    const bool animated = !visibility.isConstant();
    bool everHidden = false;
    for (size_t i = 0; i < samples; ++i) {
        int8_t value = -1;
        visibility.get(value, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        everHidden = everHidden || value == 0;
    }
    if (!everHidden) return;  // visible all along: nothing to say
    UsdAttribute attr = imageable.CreateVisibilityAttr();
    for (size_t i = 0; i < samples; ++i) {
        int8_t value = -1;
        visibility.get(value, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        attr.Set(value == 0 ? UsdGeomTokens->invisible : UsdGeomTokens->inherited, TimeOf(run, visibility, i, animated));
    }
}

void Extent(UsdGeomPointBased shape, const Abc::Box3d& bounds, const VtVec3fArray& points, const UsdTimeCode& time) {
    VtVec3fArray extent(2);
    if (!bounds.isEmpty() && !bounds.isInfinite()) {
        extent[0] = GfVec3f(static_cast<float>(bounds.min.x), static_cast<float>(bounds.min.y), static_cast<float>(bounds.min.z));
        extent[1] = GfVec3f(static_cast<float>(bounds.max.x), static_cast<float>(bounds.max.y), static_cast<float>(bounds.max.z));
    } else if (!UsdGeomPointBased::ComputeExtent(points, &extent)) {
        return;
    }
    shape.CreateExtentAttr().Set(extent, time);
}

// Polygon meshes and subdivision surfaces read the same way; the
// subdivision scheme and its creases are all that a SubD adds.
template <typename Schema>
void MeshFrom(Run& run, Schema& schema, const Abc::IObject& object, UsdGeomMesh mesh, bool subdivision) {
    const size_t samples = std::max<size_t>(schema.getNumSamples(), 1);
    const bool animated = !schema.isConstant();
    const bool topologyChanges = schema.getTopologyVariance() == AbcG::kHeterogenousTopology;
    Topology topology;
    for (size_t i = 0; i < samples && !run.cancelled; ++i) {
        if (run.progress && run.progress->cancel) run.cancelled = true;
        const Abc::ISampleSelector at(static_cast<AbcA::index_t>(i));
        typename Schema::Sample sample;
        schema.get(sample, at);
        const UsdTimeCode time = TimeOf(run, schema, i, animated);
        VtVec3fArray points;
        if (sample.getPositions()) points = ToVt<GfVec3f>(sample.getPositions()->get(), sample.getPositions()->size());
        mesh.CreatePointsAttr().Set(points, time);
        if (i == 0 || topologyChanges) {
            VtIntArray counts = ToIntArray(sample.getFaceCounts());
            VtIntArray indices = ToIntArray(sample.getFaceIndices());
            topology.Add(counts);
            Reorder(&indices, topology.At(i));
            const UsdTimeCode topologyTime = topologyChanges ? time : UsdTimeCode::Default();
            mesh.CreateFaceVertexCountsAttr().Set(counts, topologyTime);
            mesh.CreateFaceVertexIndicesAttr().Set(indices, topologyTime);
        }
        if (sample.getVelocities()) {
            mesh.CreateVelocitiesAttr().Set(ToVt<GfVec3f>(sample.getVelocities()->get(), sample.getVelocities()->size()),
                                            time);
        }
        Extent(mesh, sample.getSelfBounds(), points, time);
    }
    if (run.cancelled) return;
    if (animated) ++run.animatedMeshes;
    if (topologyChanges) ++run.changingTopology;

    UsdGeomPrimvarsAPI primvars(mesh);
    PrimvarFrom<AbcG::IV2fGeomParam, GfVec2f>(run, schema.getUVsParam(), primvars, TfToken("st"),
                                              SdfValueTypeNames->TexCoord2fArray, topology);
    bool displayColour = false;
    ArbitraryParams(run, schema.getArbGeomParams(), primvars, topology, &displayColour);

    // face sets: groups of faces, kept as subsets of the mesh
    std::vector<std::string> names;
    schema.getFaceSetNames(names);
    std::unordered_map<std::string, int> taken;
    for (const std::string& setName : names) {
        AbcG::IFaceSet faceSet = schema.getFaceSet(setName);
        AbcG::IFaceSetSchema::Sample faces;
        faceSet.getSchema().get(faces);
        if (!faces.getFaces()) continue;
        UsdGeomSubset::CreateGeomSubset(mesh, TfToken(UniqueName(PrimNameFor(setName, "faceSet"), &taken)),
                                        UsdGeomTokens->face, ToIntArray(faces.getFaces()));
        ++run.faceSets;
    }
    Visibility(run, object, mesh);
    (void)subdivision;
}

void PolyMesh(Run& run, const Abc::IObject& object, UsdGeomMesh mesh) {
    AbcG::IPolyMesh source(object, Abc::kWrapExisting);
    AbcG::IPolyMeshSchema& schema = source.getSchema();
    MeshFrom(run, schema, object, mesh, false);
    // polygons as they are: Alembic says nothing of a subdivision for a poly mesh
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    // normals: per point or per corner, into the mesh's own normals
    AbcG::IN3fGeomParam normals = schema.getNormalsParam();
    if (normals.valid()) {
        const size_t samples = std::max<size_t>(normals.getNumSamples(), 1);
        const bool animated = !normals.isConstant();
        Topology topology;
        for (size_t i = 0; i < std::max<size_t>(schema.getNumSamples(), 1); ++i) {
            AbcG::IPolyMeshSchema::Sample sample;
            schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
            topology.Add(ToIntArray(sample.getFaceCounts()));
            if (schema.getTopologyVariance() != AbcG::kHeterogenousTopology) break;
        }
        const TfToken interpolation = InterpolationOf(normals.getScope());
        for (size_t i = 0; i < samples; ++i) {
            AbcG::IN3fGeomParam::Sample sample;
            normals.getExpanded(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
            if (!sample.getVals()) continue;
            VtVec3fArray values = ToVt<GfVec3f>(sample.getVals()->get(), sample.getVals()->size());
            if (interpolation == UsdGeomTokens->faceVarying) Reorder(&values, topology.At(i));
            mesh.CreateNormalsAttr().Set(values, TimeOf(run, normals, i, animated));
        }
        mesh.SetNormalsInterpolation(interpolation);
    }
    ++run.meshes;
}

void SubD(Run& run, const Abc::IObject& object, UsdGeomMesh mesh) {
    AbcG::ISubD source(object, Abc::kWrapExisting);
    AbcG::ISubDSchema& schema = source.getSchema();
    MeshFrom(run, schema, object, mesh, true);
    AbcG::ISubDSchema::Sample sample;
    schema.get(sample);
    const std::string scheme = sample.getSubdivisionScheme();
    mesh.CreateSubdivisionSchemeAttr().Set(scheme == "loop" ? UsdGeomTokens->loop
                                           : scheme == "bilinear" ? UsdGeomTokens->bilinear
                                                                  : UsdGeomTokens->catmullClark);
    if (sample.getCreaseIndices() && sample.getCreaseIndices()->size()) {
        mesh.CreateCreaseIndicesAttr().Set(ToIntArray(sample.getCreaseIndices()));
        mesh.CreateCreaseLengthsAttr().Set(ToIntArray(sample.getCreaseLengths()));
        if (sample.getCreaseSharpnesses()) {
            mesh.CreateCreaseSharpnessesAttr().Set(
                ToVt<float>(sample.getCreaseSharpnesses()->get(), sample.getCreaseSharpnesses()->size()));
        }
    }
    if (sample.getCornerIndices() && sample.getCornerIndices()->size()) {
        mesh.CreateCornerIndicesAttr().Set(ToIntArray(sample.getCornerIndices()));
        if (sample.getCornerSharpnesses()) {
            mesh.CreateCornerSharpnessesAttr().Set(
                ToVt<float>(sample.getCornerSharpnesses()->get(), sample.getCornerSharpnesses()->size()));
        }
    }
    if (sample.getHoles() && sample.getHoles()->size()) mesh.CreateHoleIndicesAttr().Set(ToIntArray(sample.getHoles()));
    ++run.subds;
}

void Camera(Run& run, const Abc::IObject& object, UsdGeomCamera camera) {
    AbcG::ICamera source(object, Abc::kWrapExisting);
    AbcG::ICameraSchema& schema = source.getSchema();
    const size_t samples = std::max<size_t>(schema.getNumSamples(), 1);
    const bool animated = !schema.isConstant();
    for (size_t i = 0; i < samples; ++i) {
        AbcG::CameraSample sample;
        schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        const UsdTimeCode time = TimeOf(run, schema, i, animated);
        // Alembic film back in centimetres, USD in millimetres; the lens
        // squeeze widens what the lens sees horizontally
        const double squeeze = sample.getLensSqueezeRatio() > 0.0 ? sample.getLensSqueezeRatio() : 1.0;
        camera.CreateFocalLengthAttr().Set(static_cast<float>(sample.getFocalLength()), time);
        camera.CreateHorizontalApertureAttr().Set(static_cast<float>(sample.getHorizontalAperture() * 10.0 * squeeze), time);
        camera.CreateVerticalApertureAttr().Set(static_cast<float>(sample.getVerticalAperture() * 10.0), time);
        camera.CreateHorizontalApertureOffsetAttr().Set(static_cast<float>(sample.getHorizontalFilmOffset() * 10.0), time);
        camera.CreateVerticalApertureOffsetAttr().Set(static_cast<float>(sample.getVerticalFilmOffset() * 10.0), time);
        camera.CreateClippingRangeAttr().Set(
            GfVec2f(static_cast<float>(sample.getNearClippingPlane()), static_cast<float>(sample.getFarClippingPlane())), time);
        camera.CreateFStopAttr().Set(static_cast<float>(sample.getFStop()), time);
        camera.CreateFocusDistanceAttr().Set(static_cast<float>(sample.getFocusDistance()), time);
    }
    if (animated) ++run.animatedCameras;
    Visibility(run, object, camera);
    ++run.cameras;
}

void Points(Run& run, const Abc::IObject& object, UsdGeomPoints points) {
    AbcG::IPoints source(object, Abc::kWrapExisting);
    AbcG::IPointsSchema& schema = source.getSchema();
    const size_t samples = std::max<size_t>(schema.getNumSamples(), 1);
    const bool animated = !schema.isConstant();
    for (size_t i = 0; i < samples; ++i) {
        AbcG::IPointsSchema::Sample sample;
        schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        const UsdTimeCode time = TimeOf(run, schema, i, animated);
        VtVec3fArray positions;
        if (sample.getPositions()) positions = ToVt<GfVec3f>(sample.getPositions()->get(), sample.getPositions()->size());
        points.CreatePointsAttr().Set(positions, time);
        if (sample.getIds()) {
            VtInt64Array ids(sample.getIds()->size());
            for (size_t p = 0; p < ids.size(); ++p) ids[p] = static_cast<int64_t>((*sample.getIds())[p]);
            points.CreateIdsAttr().Set(ids, time);
        }
        if (sample.getVelocities()) {
            points.CreateVelocitiesAttr().Set(ToVt<GfVec3f>(sample.getVelocities()->get(), sample.getVelocities()->size()), time);
        }
        Extent(points, sample.getSelfBounds(), positions, time);
    }
    AbcG::IFloatGeomParam widths = schema.getWidthsParam();
    if (widths.valid()) {
        const size_t widthSamples = std::max<size_t>(widths.getNumSamples(), 1);
        for (size_t i = 0; i < widthSamples; ++i) {
            AbcG::IFloatGeomParam::Sample sample;
            widths.getExpanded(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
            if (sample.getVals()) {
                points.CreateWidthsAttr().Set(ToVt<float>(sample.getVals()->get(), sample.getVals()->size()),
                                              TimeOf(run, widths, i, !widths.isConstant()));
            }
        }
        points.SetWidthsInterpolation(InterpolationOf(widths.getScope()));
    }
    bool displayColour = false;
    ArbitraryParams(run, schema.getArbGeomParams(), UsdGeomPrimvarsAPI(points), Topology(), &displayColour);
    Visibility(run, object, points);
    ++run.points;
}

void Curves(Run& run, const Abc::IObject& object, UsdGeomBasisCurves curves) {
    AbcG::ICurves source(object, Abc::kWrapExisting);
    AbcG::ICurvesSchema& schema = source.getSchema();
    const size_t samples = std::max<size_t>(schema.getNumSamples(), 1);
    const bool animated = !schema.isConstant();
    for (size_t i = 0; i < samples; ++i) {
        AbcG::ICurvesSchema::Sample sample;
        schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        const UsdTimeCode time = TimeOf(run, schema, i, animated);
        VtVec3fArray positions;
        if (sample.getPositions()) positions = ToVt<GfVec3f>(sample.getPositions()->get(), sample.getPositions()->size());
        curves.CreatePointsAttr().Set(positions, time);
        curves.CreateCurveVertexCountsAttr().Set(ToIntArray(sample.getCurvesNumVertices()), time);
        if (i == 0) {
            curves.CreateTypeAttr().Set(sample.getType() == AbcG::kLinear ? UsdGeomTokens->linear : UsdGeomTokens->cubic);
            curves.CreateWrapAttr().Set(sample.getWrap() == AbcG::kPeriodic ? UsdGeomTokens->periodic
                                                                             : UsdGeomTokens->nonperiodic);
            const AbcG::BasisType basis = sample.getBasis();
            curves.CreateBasisAttr().Set(basis == AbcG::kBezierBasis       ? UsdGeomTokens->bezier
                                         : basis == AbcG::kCatmullromBasis ? UsdGeomTokens->catmullRom
                                                                           : UsdGeomTokens->bspline);
        }
        Extent(curves, sample.getSelfBounds(), positions, time);
    }
    AbcG::IFloatGeomParam widths = schema.getWidthsParam();
    if (widths.valid()) {
        AbcG::IFloatGeomParam::Sample sample;
        widths.getExpanded(sample);
        if (sample.getVals()) curves.CreateWidthsAttr().Set(ToVt<float>(sample.getVals()->get(), sample.getVals()->size()));
        curves.SetWidthsInterpolation(InterpolationOf(widths.getScope()));
    }
    Visibility(run, object, curves);
    ++run.curves;
}

// ------------------------------------------------------------- transforms

void Transform(Run& run, const Abc::IObject& object, UsdGeomXformable xformable) {
    AbcG::IXform source(object, Abc::kWrapExisting);
    AbcG::IXformSchema& schema = source.getSchema();
    ++run.transforms;
    if (schema.isConstantIdentity() && schema.getInheritsXforms()) return;
    const size_t samples = std::max<size_t>(schema.getNumSamples(), 1);
    const bool animated = !schema.isConstant();
    UsdGeomXformOp op = xformable.AddTransformOp(UsdGeomXformOp::PrecisionDouble);
    bool resets = false;
    for (size_t i = 0; i < samples; ++i) {
        AbcG::XformSample sample;
        schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(i)));
        const Abc::M44d m = sample.getMatrix();
        GfMatrix4d matrix;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) matrix[r][c] = m[r][c];  // both row-vector, translation in the last row
        }
        op.Set(matrix, TimeOf(run, schema, i, animated));
        resets = resets || !sample.getInheritsXforms();
    }
    if (resets) xformable.SetResetXformStack(true);
    if (animated) ++run.animatedTransforms;
    Visibility(run, object, xformable);
}

// --------------------------------------------------------------- hierarchy

enum class Kind { Xform, Mesh, SubD, Camera, Points, Curves, Other };

Kind KindOf(const AbcA::ObjectHeader& header, std::string* other) {
    if (AbcG::IXform::matches(header)) return Kind::Xform;
    if (AbcG::IPolyMesh::matches(header)) return Kind::Mesh;
    if (AbcG::ISubD::matches(header)) return Kind::SubD;
    if (AbcG::ICamera::matches(header)) return Kind::Camera;
    if (AbcG::IPoints::matches(header)) return Kind::Points;
    if (AbcG::ICurves::matches(header)) return Kind::Curves;
    if (AbcG::INuPatch::matches(header)) {
        *other = "NURBS surface";
    } else if (AbcG::ILight::matches(header)) {
        *other = "light";
    } else if (AbcG::IFaceSet::matches(header)) {
        *other = "";  // read with its mesh
    } else {
        const std::string schema = header.getMetaData().get("schema");
        *other = schema.empty() ? "group" : schema;
    }
    return Kind::Other;
}

bool IsShape(Kind kind) {
    return kind == Kind::Mesh || kind == Kind::SubD || kind == Kind::Camera || kind == Kind::Points ||
           kind == Kind::Curves;
}

// Writes a shape (or anything that is not a transform) at `path`.
void Shape(Run& run, const Abc::IObject& object, Kind kind, const SdfPath& path) {
    switch (kind) {
    case Kind::Mesh: PolyMesh(run, object, UsdGeomMesh::Define(run.stage, path)); break;
    case Kind::SubD: SubD(run, object, UsdGeomMesh::Define(run.stage, path)); break;
    case Kind::Camera: Camera(run, object, UsdGeomCamera::Define(run.stage, path)); break;
    case Kind::Points: Points(run, object, UsdGeomPoints::Define(run.stage, path)); break;
    case Kind::Curves: Curves(run, object, UsdGeomBasisCurves::Define(run.stage, path)); break;
    default: break;
    }
}

void Children(Run& run, const Abc::IObject& parent, const SdfPath& parentPath);

// Nothing below the object but its face sets (which its mesh reads).
bool OnlyFaceSetsBelow(const Abc::IObject& object) {
    for (size_t i = 0; i < object.getNumChildren(); ++i) {
        if (!AbcG::IFaceSet::matches(object.getChild(i).getHeader())) return false;
    }
    return true;
}

void Object(Run& run, const Abc::IObject& object, const SdfPath& parentPath, std::unordered_map<std::string, int>* taken) {
    if (run.cancelled) return;
    std::string other;
    const Kind kind = KindOf(object.getHeader(), &other);
    if (kind == Kind::Other && other.empty()) return;  // a face set: its mesh reads it
    const SdfPath path = parentPath.AppendChild(TfToken(UniqueName(PrimNameFor(object.getName(), "object"), taken)));

    if (kind == Kind::Xform) {
        // A transform with one shape below it and nothing else (face sets
        // aside) is one object: one prim, named after the transform (as
        // usdAbc does).
        if (object.getNumChildren() == 1) {
            const Abc::IObject child = object.getChild(0);
            std::string childOther;
            const Kind childKind = KindOf(child.getHeader(), &childOther);
            if (IsShape(childKind) && OnlyFaceSetsBelow(child)) {
                Shape(run, child, childKind, path);
                Transform(run, object, UsdGeomXformable(run.stage->GetPrimAtPath(path)));
                run.Tick();
                run.Tick();
                return;
            }
        }
        Transform(run, object, UsdGeomXform::Define(run.stage, path));
        run.Tick();
        Children(run, object, path);
        return;
    }
    if (IsShape(kind)) {
        Shape(run, object, kind, path);
        run.Tick();
        Children(run, object, path);  // a shape with children is unusual, but they are kept
        return;
    }
    // a kind USDprep does not read: the place stays (its children may be readable), the thing is named
    ++run.leftOut[other];
    UsdGeomXform::Define(run.stage, path);
    run.Tick();
    Children(run, object, path);
}

void Children(Run& run, const Abc::IObject& parent, const SdfPath& parentPath) {
    std::unordered_map<std::string, int> taken;
    for (size_t i = 0; i < parent.getNumChildren() && !run.cancelled; ++i) {
        Object(run, parent.getChild(i), parentPath, &taken);
    }
}

size_t CountObjects(const Abc::IObject& object) {
    size_t count = 0;
    for (size_t i = 0; i < object.getNumChildren(); ++i) count += 1 + CountObjects(object.getChild(i));
    return count;
}

// The archive's frame rate: the uniform time sampling most samples use.
double FramesPerSecond(Abc::IArchive& archive) {
    double best = 0.0;
    AbcA::index_t mostSamples = 0;
    for (uint32_t i = 1; i < archive.getNumTimeSamplings(); ++i) {
        const AbcA::TimeSamplingPtr sampling = archive.getTimeSampling(i);
        if (!sampling) continue;
        const AbcA::TimeSamplingType type = sampling->getTimeSamplingType();
        if (!type.isUniform() || type.getTimePerCycle() <= 0.0) continue;
        const AbcA::index_t samples = archive.getMaxNumSamplesForTimeSamplingIndex(i);
        if (samples > mostSamples || best == 0.0) {
            mostSamples = samples;
            best = 1.0 / type.getTimePerCycle();
        }
    }
    if (best <= 0.0) return 24.0;
    // 24.000000002 is 24; 23.976 stays 23.976
    if (std::fabs(best - std::round(best)) < 1e-3) best = std::round(best);
    return best;
}

bool LooksLikeHdf5(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    char head[8] = {};
    file.read(head, sizeof(head));
    return file.gcount() == 8 && std::memcmp(head, "\x89HDF\r\n\x1a\n", 8) == 0;
}

std::string Plural(size_t n, const char* one) {
    const std::string word = one;
    if (n == 1) return Count(n) + " " + word;
    const bool es = word.size() > 1 && (word.back() == 's' || word.back() == 'x' ||
                                        word.compare(word.size() - 2, 2, "sh") == 0 ||
                                        word.compare(word.size() - 2, 2, "ch") == 0);
    return Count(n) + " " + word + (es ? "es" : "s");
}

}  // namespace

Report ImportAbc(const std::string& inputPath, const std::string& usdPath, Progress* progress) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    const fs::path source = PathOf(inputPath);
    if (!IsFile(source)) {
        rep.Fail("cannot read " + inputPath + ": the file is not there");
        return rep;
    }
    if (progress) {
        progress->step = "reading the Alembic file";
        progress->fraction = 0.0f;
    }

    // Alembic reads through a stream we open: the path may hold any characters
    std::ifstream stream(source, std::ios::binary);
    if (!stream) {
        rep.Fail("cannot open " + inputPath + " for reading");
        return rep;
    }
    std::vector<std::istream*> streams{&stream};
    Alembic::AbcCoreFactory::IFactory factory;
    factory.setPolicy(Abc::ErrorHandler::kThrowPolicy);
    Alembic::AbcCoreFactory::IFactory::CoreType core;
    Abc::IArchive archive;
    try {
        archive = factory.getArchive(streams, core);
    } catch (const std::exception& e) {
        rep.Fail("cannot read " + inputPath + ": not an Alembic archive, or " + ReadableError(e.what()));
        return rep;
    }
    if (!archive.valid()) {
        rep.Fail(LooksLikeHdf5(source)
                     ? "cannot read " + inputPath +
                           ": an HDF5 Alembic (the format before 2013) - export it again as Ogawa, which "
                           "every current application writes"
                     : "cannot read " + inputPath + ": not an Alembic archive, or a damaged one");
        return rep;
    }

    Run run;
    run.rep = &rep;
    run.progress = progress;
    run.stage = UsdStage::CreateInMemory();
    run.fps = FramesPerSecond(archive);
    UsdGeomSetStageUpAxis(run.stage, UsdGeomTokens->y);  // Alembic says nothing; the applications that write it are Y-up
    const std::string stem = Utf8Of(source.stem());
    const std::string rootName = PrimNameFor(stem, "alembic");
    UsdGeomXform root = UsdGeomXform::Define(run.stage, SdfPath::AbsoluteRootPath().AppendChild(TfToken(rootName)));
    run.stage->SetDefaultPrim(root.GetPrim());
    MarkImported(run.stage, source);

    const Abc::IObject top = archive.getTop();
    run.objectsTotal = CountObjects(top);
    if (progress) {
        progress->step = "converting the objects";
        progress->fraction = 0.05f;
    }
    Children(run, top, root.GetPath());
    if (run.cancelled) {
        rep.Fail("cancelled");
        return rep;
    }

    // the archive's frames, at its own rate
    run.stage->SetTimeCodesPerSecond(run.fps);
    run.stage->SetFramesPerSecond(run.fps);
    const bool animated = run.lastFrame >= run.firstFrame && run.lastFrame > run.firstFrame;
    if (run.lastFrame >= run.firstFrame) {
        run.stage->SetStartTimeCode(run.firstFrame);
        run.stage->SetEndTimeCode(run.lastFrame);
    }

    // ----- the report
    std::string what;
    const auto add = [&](size_t n, const char* one) {
        if (n == 0) return;
        what += (what.empty() ? "" : ", ") + Plural(n, one);
    };
    add(run.meshes, "mesh");
    add(run.subds, "subdivision surface");
    add(run.cameras, "camera");
    add(run.points, "point cloud");
    add(run.curves, "curve set");
    rep.Info("import", Utf8Of(source.filename()) + ": " + (what.empty() ? std::string("no geometry") : what) +
                           (animated ? ", frames " + Number(run.firstFrame) + "-" + Number(run.lastFrame) + " at " +
                                           Number(run.fps) + " fps"
                                     : ", no animation"));
    if (run.animatedMeshes || run.animatedTransforms || run.animatedCameras) {
        std::string moving;
        if (run.animatedMeshes) moving += Plural(run.animatedMeshes, "point cache");
        if (run.animatedTransforms) moving += (moving.empty() ? "" : ", ") + Plural(run.animatedTransforms, "moving transform");
        if (run.animatedCameras) moving += (moving.empty() ? "" : ", ") + Plural(run.animatedCameras, "animated camera");
        rep.Info("import", "animation kept, every sample: " + moving);
    }
    if (run.changingTopology) {
        rep.Info("import", Plural(run.changingTopology, "mesh") +
                               " change their polygons from frame to frame (a simulation) - kept as they are; such "
                               "meshes cannot be reduced");
    }
    if (!run.colourSets.empty()) {
        rep.Info("import", "vertex colours (" + *run.colourSets.begin() +
                               (run.colourSets.size() > 1 ? ", ..." : "") +
                               ") also kept as display colour - what Nuke shows without a material");
    }
    if (run.faceSets) rep.Info("import", Plural(run.faceSets, "face set") + " kept as subsets of their meshes");
    if (run.meshes + run.subds > 0) {
        rep.Info("import", "Alembic carries no materials: the objects come with their display colour, or grey");
    }
    if (run.points || run.curves) {
        rep.Warn("import", "point clouds and curves come along, but Nuke's USD reader draws neither");
    }
    for (const auto& entry : run.leftOut) {
        rep.Warn("import", Plural(entry.second, entry.first.c_str()) + " left out - USDprep reads meshes, "
                                                                        "subdivision surfaces, cameras, points and curves");
    }
    if (!run.paramsLeftOut.empty()) {
        rep.Warn("import", "data of a type USDprep does not read left out: " + *run.paramsLeftOut.begin() +
                               (run.paramsLeftOut.size() > 1 ? ", ..." : ""));
    }

    if (progress) {
        progress->step = "writing the USD file";
        progress->fraction = 0.9f;
    }
    if (!SaveImported(run.stage, usdPath, rep)) return rep;
    rep.after.prims = run.objectsDone;
    rep.after.meshes = run.meshes + run.subds;
    rep.after.cameras = run.cameras;
    rep.ok = true;
    if (progress) progress->fraction = 1.0f;
    return rep;
}

}  // namespace importer
}  // namespace usdprep

#else  // !USDPREP_WITH_ALEMBIC

namespace usdprep {
namespace importer {

Report ImportAbc(const std::string& inputPath, const std::string& usdPath, Progress*) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    rep.Fail("this build of USDprep has no Alembic in it (.abc needs the Alembic library at build time)");
    return rep;
}

}  // namespace importer
}  // namespace usdprep

#endif
