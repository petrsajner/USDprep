// What the mesh-file writers (.obj, .abc) are handed: meshes already in
// world space, UVs per corner, sampled by whoever reads the USD.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>

namespace usdprep {
namespace detail {

struct MeshData {
    std::string name;
    pxr::VtVec3fArray points;  // world space
    pxr::VtVec2fArray uvs;     // one per corner, empty = none
    pxr::VtIntArray counts, indices;
    bool flip = false;         // leftHanded: reverse the winding
};

struct AbcMesh {
    std::string name;
    bool animated = false;  // false: one sample, whatever the frame list says
};

// Fills `out` with mesh number `index` at `frame`. False = skip the sample.
using MeshSampler = std::function<bool(size_t index, double frame, MeshData* out)>;

// AbcWriter.cpp. One Ogawa archive, a PolyMesh per entry of `meshes`,
// positions sampled at every frame of `frames` for the animated ones.
// `objectPaths` receives the objects as Nuke's ReadGeo lists them.
// False (and `error`) when the file could not be written - or when this
// build has no Alembic in it.
bool WriteAlembic(const std::string& path, const std::vector<AbcMesh>& meshes,
                  const std::vector<size_t>& which, const std::vector<double>& frames, double framesPerSecond,
                  const MeshSampler& sample, std::vector<std::string>* objectPaths, std::string* error);

}  // namespace detail
}  // namespace usdprep
