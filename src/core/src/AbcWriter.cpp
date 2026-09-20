// The Alembic side of the mesh export, kept apart so that the rest of the
// core builds without the library (USDPREP_WITH_ALEMBIC).
//
// What Nuke's classic ReadGeo takes from an .abc (measured): poly meshes,
// their UVs, and their animation as samples of the positions. So that is
// all that is written: world-space positions per frame on a uniform time
// sampling, topology and face-varying UVs once.

#include "MeshExport.h"

#ifdef USDPREP_WITH_ALEMBIC

#include <memory>
#include <set>

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

namespace usdprep {
namespace detail {

namespace {

namespace Abc = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;

// Alembic winds its faces clockwise, USD counter-clockwise: every face is
// written backwards (a leftHanded USD mesh already is).
void Rewind(const MeshData& mesh, std::vector<int32_t>* indices, std::vector<Imath::V2f>* uvs) {
    indices->resize(mesh.indices.size());
    uvs->resize(mesh.uvs.size());
    size_t corner = 0;
    for (const int count : mesh.counts) {
        for (int k = 0; k < count; ++k) {
            const size_t from = corner + (mesh.flip ? k : count - 1 - k);
            (*indices)[corner + k] = mesh.indices[from];
            if (!mesh.uvs.empty()) (*uvs)[corner + k] = Imath::V2f(mesh.uvs[from][0], mesh.uvs[from][1]);
        }
        corner += static_cast<size_t>(count);
    }
}

std::string ObjectName(const std::string& text, std::set<std::string>* taken) {
    std::string name;
    for (const char c : text) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (name.empty()) name = "mesh";
    std::string unique = name;
    for (int n = 2; !taken->insert(unique).second; ++n) unique = name + "_" + std::to_string(n);
    return unique;
}

}  // namespace

bool WriteAlembic(const std::string& path, const std::vector<AbcMesh>& meshes,
                  const std::vector<size_t>& which, const std::vector<double>& frames, double framesPerSecond,
                  const MeshSampler& sample, std::vector<std::string>* objectPaths, std::string* error) {
    if (frames.empty() || which.empty()) {
        *error = "nothing to write";
        return false;
    }
    try {
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
        const double fps = framesPerSecond > 0.0 ? framesPerSecond : 24.0;
        // frame N sits at N / fps seconds, which is how Nuke maps it back
        const uint32_t timeIndex =
            archive.addTimeSampling(Abc::TimeSampling(1.0 / fps, frames.front() / fps));

        struct Out {
            size_t index;
            std::unique_ptr<AbcGeom::OXform> xform;
            std::unique_ptr<AbcGeom::OPolyMesh> mesh;
        };
        std::vector<Out> outs;
        std::set<std::string> taken;
        for (const size_t index : which) {
            Out out;
            out.index = index;
            const std::string name = ObjectName(meshes[index].name, &taken);
            out.xform.reset(new AbcGeom::OXform(archive.getTop(), name, timeIndex));
            // world space is in the points; the transform is there because readers expect one
            AbcGeom::XformSample identity;
            out.xform->getSchema().set(identity);
            out.mesh.reset(new AbcGeom::OPolyMesh(*out.xform, name + "Shape", timeIndex));
            if (objectPaths) objectPaths->push_back("/root/" + name + "/" + name + "Shape");  // as Nuke lists it
            outs.push_back(std::move(out));
        }

        std::vector<int32_t> indices;
        std::vector<Imath::V2f> uvs;
        for (size_t f = 0; f < frames.size(); ++f) {
            for (Out& out : outs) {
                if (f > 0 && !meshes[out.index].animated) continue;
                MeshData data;
                if (!sample(out.index, frames[f], &data)) continue;
                AbcGeom::OPolyMeshSchema& schema = out.mesh->getSchema();
                const Abc::P3fArraySample positions(
                    reinterpret_cast<const Imath::V3f*>(data.points.cdata()), data.points.size());
                if (f == 0) {
                    Rewind(data, &indices, &uvs);
                    AbcGeom::OV2fGeomParam::Sample uvSample;
                    if (!uvs.empty()) {
                        uvSample = AbcGeom::OV2fGeomParam::Sample(Abc::V2fArraySample(uvs.data(), uvs.size()),
                                                                  AbcGeom::kFacevaryingScope);
                    }
                    const std::vector<int32_t> counts(data.counts.begin(), data.counts.end());
                    schema.set(AbcGeom::OPolyMeshSchema::Sample(
                        positions, Abc::Int32ArraySample(indices.data(), indices.size()),
                        Abc::Int32ArraySample(counts.data(), counts.size()), uvSample));
                } else {
                    AbcGeom::OPolyMeshSchema::Sample next;
                    next.setPositions(positions);
                    schema.set(next);
                }
            }
        }
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
    return true;
}

}  // namespace detail
}  // namespace usdprep

#else  // !USDPREP_WITH_ALEMBIC

namespace usdprep {
namespace detail {

bool WriteAlembic(const std::string&, const std::vector<AbcMesh>&, const std::vector<size_t>&,
                  const std::vector<double>&, double, const MeshSampler&, std::vector<std::string>*,
                  std::string* error) {
    *error = "this build of usdprep has no Alembic in it (.abc needs the Alembic library at build time)";
    return false;
}

}  // namespace detail
}  // namespace usdprep

#endif
