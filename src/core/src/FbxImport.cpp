// FBX in: an .fbx turned into USD, read with assimp.
//
// What a 3D application shows is what comes out: the node hierarchy with
// its transforms and their animation on the file's own frames, meshes with
// their polygons as they are (not triangulated), UV sets, normals, vertex
// colours, the materials with their textures - also the ones packed inside
// the .fbx - cameras and lights. Skinned and blend-shaped meshes come out
// deformed, frame by frame, as point caches: what Nuke needs, and what the
// Nuke export would bake them into anyway.
//
// assimp hands a mesh over split by material and with one vertex per
// polygon corner: the parts of one node are joined again into one mesh
// with a subset per material, its corners welded where they share a
// position. Pivot helper nodes assimp may add ("$AssimpFbx$") are folded
// into the node they belong to.

#include "ImportShared.h"
#include "FbxScan.h"

#ifdef USDPREP_WITH_ASSIMP

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/scene.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>

#include <usdprep/Import.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdprep {
namespace importer {

namespace {

// --------------------------------------------------------------- matrices

// assimp multiplies column vectors (translation in the last column), USD
// row vectors (translation in the last row): the one is the other turned.
GfMatrix4d ToUsd(const aiMatrix4x4& m) {
    return GfMatrix4d(m.a1, m.b1, m.c1, m.d1,
                      m.a2, m.b2, m.c2, m.d2,
                      m.a3, m.b3, m.c3, m.d3,
                      m.a4, m.b4, m.c4, m.d4);
}

bool IsHelper(const aiNode* node) { return std::strstr(node->mName.C_Str(), "$AssimpFbx$") != nullptr; }

// ------------------------------------------------------------- animation

template <typename Key>
size_t KeyBefore(const Key* keys, unsigned count, double time) {
    // the last key at or before `time` (0 before the first)
    size_t low = 0, high = count;
    while (high - low > 1) {
        const size_t mid = (low + high) / 2;
        if (keys[mid].mTime <= time) low = mid; else high = mid;
    }
    return low;
}

aiVector3D VectorAt(const aiVectorKey* keys, unsigned count, double time, const aiVector3D& fallback) {
    if (count == 0) return fallback;
    if (count == 1 || time <= keys[0].mTime) return keys[0].mValue;
    if (time >= keys[count - 1].mTime) return keys[count - 1].mValue;
    const size_t i = KeyBefore(keys, count, time);
    const aiVectorKey& a = keys[i];
    const aiVectorKey& b = keys[i + 1];
    if (a.mInterpolation == aiAnimInterpolation_Step || b.mTime <= a.mTime) return a.mValue;
    const ai_real f = static_cast<ai_real>((time - a.mTime) / (b.mTime - a.mTime));
    return a.mValue + (b.mValue - a.mValue) * f;
}

aiQuaternion QuatAt(const aiQuatKey* keys, unsigned count, double time, const aiQuaternion& fallback) {
    if (count == 0) return fallback;
    if (count == 1 || time <= keys[0].mTime) return keys[0].mValue;
    if (time >= keys[count - 1].mTime) return keys[count - 1].mValue;
    const size_t i = KeyBefore(keys, count, time);
    const aiQuatKey& a = keys[i];
    const aiQuatKey& b = keys[i + 1];
    if (a.mInterpolation == aiAnimInterpolation_Step || b.mTime <= a.mTime) return a.mValue;
    aiQuaternion out;
    aiQuaternion::Interpolate(out, a.mValue, b.mValue, static_cast<ai_real>((time - a.mTime) / (b.mTime - a.mTime)));
    return out.Normalize();
}

// ------------------------------------------------------------------ run

struct Run {
    const aiScene* scene = nullptr;
    UsdStageRefPtr stage;
    Report* rep = nullptr;
    Progress* progress = nullptr;
    fs::path source;
    fs::path embeddedDir;  // textures packed inside the .fbx are written here
    double fps = 24.0;
    double ticksPerFrame = 1.0;  // assimp's ticks for one frame
    double offsetTicks = 0.0;    // assimp starts every animation at 0; where it really starts
    std::vector<double> ticks;   // the frames sampled, in ticks; empty = nothing moves
    std::unordered_map<std::string, const aiNodeAnim*> channels;
    std::unordered_map<std::string, std::vector<const aiMeshMorphAnim*>> morphs;
    std::unordered_map<std::string, const aiCamera*> cameras;
    std::unordered_map<std::string, const aiLight*> lights;
    std::unordered_map<std::string, const aiNode*> nodes;
    std::vector<SdfPath> materialPaths;  // per scene material, written when first used
    SdfPath materialsScope;
    PreviewMaterialWriter* writer = nullptr;
    std::unordered_map<std::string, std::string> resolved;  // texture as written -> as found
    size_t nodesTotal = 0, nodesDone = 0;
    bool cancelled = false;
    // for the report
    size_t meshes = 0, polygons = 0, skinned = 0, morphed = 0, animatedNodes = 0, cameraCount = 0, lightCount = 0;
    size_t embedded = 0, linesAndPoints = 0, subsetMeshes = 0;
    std::vector<std::string> foundElsewhere, missing, heightMaps;

    // assimp's tick (counted from the animation's start) as the file's frame number
    double FrameOf(double tick) const { return std::round((tick + offsetTicks) / ticksPerFrame * 10000.0) / 10000.0; }

    bool Tick() {
        ++nodesDone;
        if (progress) {
            if (progress->cancel) cancelled = true;
            const float at = 0.1f + 0.8f * static_cast<float>(nodesDone) / static_cast<float>(std::max<size_t>(nodesTotal, 1));
            if (at > progress->fraction) progress->fraction = at;
        }
        return !cancelled;
    }

    // A node's own transform at `tick`, assimp's way round.
    aiMatrix4x4 LocalAt(const aiNode* node, double tick) const {
        const auto found = channels.find(node->mName.C_Str());
        if (found == channels.end() || tick != tick) return node->mTransformation;
        const aiNodeAnim* channel = found->second;
        aiVector3D scaling, position;
        aiQuaternion rotation;
        node->mTransformation.Decompose(scaling, rotation, position);
        return aiMatrix4x4(VectorAt(channel->mScalingKeys, channel->mNumScalingKeys, tick, scaling),
                           QuatAt(channel->mRotationKeys, channel->mNumRotationKeys, tick, rotation),
                           VectorAt(channel->mPositionKeys, channel->mNumPositionKeys, tick, position));
    }

    aiMatrix4x4 GlobalAt(const aiNode* node, double tick) const {
        aiMatrix4x4 m = LocalAt(node, tick);
        for (const aiNode* up = node->mParent; up; up = up->mParent) m = LocalAt(up, tick) * m;
        return m;
    }

    bool Moves(const aiNode* node) const { return channels.count(node->mName.C_Str()) > 0; }
};

// ---------------------------------------------------------------- textures

bool LooksLikeNormalMap(const std::string& file) {
    const std::string name = Lower(PathOf(file).filename().u8string());
    return name.find("normal") != std::string::npos || name.find("nrm") != std::string::npos ||
           name.find("_nor") != std::string::npos || name.find("_n.") != std::string::npos;
}

// A texture packed inside the .fbx, written out next to the converted file.
std::string Unpack(Run& run, const aiTexture* texture, const std::string& written) {
    std::error_code ec;
    fs::create_directories(run.embeddedDir, ec);
    std::string name = Utf8Of(PathOf(texture->mFilename.length ? texture->mFilename.C_Str() : written).filename());
    if (name.empty() || name[0] == '*') name = "texture" + std::to_string(run.embedded);
    fs::path target = run.embeddedDir / PathOf(name);
    if (texture->mHeight == 0) {
        // compressed (png, jpg, tga...): the bytes as they are
        if (target.extension().empty()) target += "." + std::string(texture->achFormatHint);
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(texture->pcData), static_cast<std::streamsize>(texture->mWidth));
        if (!out) return "";
    } else {
        // raw BGRA texels: written as a PNG
        target.replace_extension(".png");
        std::vector<uint8_t> rgba(static_cast<size_t>(texture->mWidth) * texture->mHeight * 4);
        for (size_t i = 0; i < static_cast<size_t>(texture->mWidth) * texture->mHeight; ++i) {
            const aiTexel& t = texture->pcData[i];
            rgba[i * 4 + 0] = t.r;
            rgba[i * 4 + 1] = t.g;
            rgba[i * 4 + 2] = t.b;
            rgba[i * 4 + 3] = t.a;
        }
        HioImageSharedPtr image = HioImage::OpenForWriting(Utf8Of(target));
        HioImage::StorageSpec spec;
        spec.width = static_cast<int>(texture->mWidth);
        spec.height = static_cast<int>(texture->mHeight);
        spec.format = HioFormatUNorm8Vec4;
        spec.flipped = false;
        spec.data = rgba.data();
        if (!image || !image->Write(spec)) return "";
    }
    ++run.embedded;
    return target.lexically_normal().generic_u8string();
}

// Where a texture an .fbx names really is: packed inside it, where it
// says, or - written on another computer - by its file name next to the
// .fbx, in its .fbm folder, in a textures folder, one folder up, or
// anywhere a few levels below the .fbx.
std::string ResolveTexture(Run& run, const std::string& written) {
    if (written.empty()) return "";
    const auto known = run.resolved.find(written);
    if (known != run.resolved.end()) return known->second;
    std::string result;
    if (const aiTexture* texture = run.scene->GetEmbeddedTexture(written.c_str())) {
        result = Unpack(run, texture, written);
    }
    if (result.empty()) {
        std::string cleaned = written;
        std::replace(cleaned.begin(), cleaned.end(), '\\', '/');
        fs::path path = PathOf(cleaned);
        const fs::path dir = run.source.parent_path();
        const fs::path name = path.filename();
        if (path.is_relative()) path = dir / path;
        const std::vector<fs::path> candidates{path,
                                               dir / name,
                                               dir / PathOf(Utf8Of(run.source.stem()) + ".fbm") / name,
                                               dir / "textures" / name,
                                               dir / "Textures" / name,
                                               dir.parent_path() / name,
                                               dir.parent_path() / "textures" / name};
        for (size_t i = 0; i < candidates.size() && result.empty(); ++i) {
            if (IsFile(candidates[i])) {
                result = candidates[i].lexically_normal().generic_u8string();
                if (i > 0) run.foundElsewhere.push_back(Utf8Of(name));
            }
        }
        if (result.empty()) {
            // a few levels below the .fbx, by name, in any letter case
            const std::string wanted = Lower(Utf8Of(name));
            std::error_code ec;
            fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end && result.empty(); it.increment(ec)) {
                if (it.depth() > 3) {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code fileError;
                if (it->is_regular_file(fileError) && Lower(Utf8Of(it->path().filename())) == wanted) {
                    result = it->path().lexically_normal().generic_u8string();
                    run.foundElsewhere.push_back(Utf8Of(name));
                }
            }
        }
        if (result.empty()) {
            run.missing.push_back(written);
            result = path.lexically_normal().generic_u8string();  // the export names it as missing
        }
    }
    run.resolved[written] = result;
    return result;
}

// ---------------------------------------------------------------- materials

bool TextureOf(Run& run, const aiMaterial* material, aiTextureType type, MapRef* map, unsigned* uvIndex) {
    if (material->GetTextureCount(type) == 0) return false;
    aiString path;
    unsigned uv = 0;
    if (material->GetTexture(type, 0, &path, nullptr, &uv) != AI_SUCCESS || path.length == 0) return false;
    map->file = ResolveTexture(run, path.C_Str());
    aiUVTransform transform;
    if (material->Get(AI_MATKEY_UVTRANSFORM(type, 0), transform) == AI_SUCCESS) {
        map->scale = GfVec2f(transform.mScaling.x, transform.mScaling.y);
        map->offset = GfVec2f(transform.mTranslation.x, transform.mTranslation.y);
    }
    if (uvIndex) *uvIndex = uv;
    return true;
}

PreviewMaterial PreviewOf(Run& run, const aiMaterial* material, const std::string& name) {
    PreviewMaterial m;
    unsigned uv = 0;
    aiColor4D colour;
    if (material->Get(AI_MATKEY_BASE_COLOR, colour) == AI_SUCCESS ||
        material->Get(AI_MATKEY_COLOR_DIFFUSE, colour) == AI_SUCCESS) {
        m.hasDiffuse = true;
        m.diffuse = GfVec3f(colour.r, colour.g, colour.b);
    }
    if (!TextureOf(run, material, aiTextureType_BASE_COLOR, &m.diffuseMap, &uv)) {
        TextureOf(run, material, aiTextureType_DIFFUSE, &m.diffuseMap, &uv);
    }
    float value = 1.0f;
    if (material->Get(AI_MATKEY_OPACITY, value) == AI_SUCCESS && value < 1.0f && value > 0.0f) {
        m.hasOpacity = true;
        m.opacity = value;
    }
    TextureOf(run, material, aiTextureType_OPACITY, &m.opacityMap, nullptr);
    if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == AI_SUCCESS) {
        m.hasRoughness = true;
        m.roughness = std::clamp(value, 0.0f, 1.0f);
    } else if (material->Get(AI_MATKEY_SHININESS, value) == AI_SUCCESS && value > 0.0f) {
        // a Phong exponent (0-1000) as roughness, the way Blender converts it
        m.hasRoughness = true;
        m.roughness = 1.0f - std::sqrt(std::clamp(value, 0.0f, 1000.0f) / 1000.0f);
    }
    if (!TextureOf(run, material, aiTextureType_DIFFUSE_ROUGHNESS, &m.roughnessMap, nullptr)) {
        // Blender and others put roughness in the "shininess" slot; a real
        // glossiness map is roughness upside down
        MapRef shininess;
        if (TextureOf(run, material, aiTextureType_SHININESS, &shininess, nullptr)) {
            const std::string file = Lower(PathOf(shininess.file).filename().u8string());
            shininess.invert = file.find("rough") == std::string::npos;
            m.roughnessMap = shininess;
        }
    }
    if (material->Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS) {
        m.hasMetallic = true;
        m.metallic = std::clamp(value, 0.0f, 1.0f);
    }
    TextureOf(run, material, aiTextureType_METALNESS, &m.metallicMap, nullptr);
    aiColor3D emissive;
    if (material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        float intensity = 1.0f;
        material->Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
        m.hasEmissive = true;
        m.emissive = GfVec3f(emissive.r, emissive.g, emissive.b) * intensity;
    }
    if (!TextureOf(run, material, aiTextureType_EMISSION_COLOR, &m.emissiveMap, nullptr)) {
        TextureOf(run, material, aiTextureType_EMISSIVE, &m.emissiveMap, nullptr);
    }
    if (!TextureOf(run, material, aiTextureType_NORMALS, &m.normalMap, nullptr) &&
        !TextureOf(run, material, aiTextureType_NORMAL_CAMERA, &m.normalMap, nullptr)) {
        MapRef height;
        if (TextureOf(run, material, aiTextureType_HEIGHT, &height, nullptr)) {
            if (LooksLikeNormalMap(height.file)) {
                m.normalMap = height;
            } else {
                run.heightMaps.push_back(name);
            }
        }
    }
    m.uvSet = uv == 0 ? "st" : "st" + std::to_string(uv);
    return m;
}

SdfPath MaterialFor(Run& run, unsigned index) {
    if (index >= run.materialPaths.size()) return SdfPath();
    if (!run.materialPaths[index].IsEmpty()) return run.materialPaths[index];
    const aiMaterial* material = run.scene->mMaterials[index];
    aiString aiName;
    material->Get(AI_MATKEY_NAME, aiName);
    // the grey material assimp makes up for geometry that has none: none it stays
    if (std::strcmp(aiName.C_Str(), AI_DEFAULT_MATERIAL_NAME) == 0 && material->GetTextureCount(aiTextureType_DIFFUSE) == 0 &&
        material->GetTextureCount(aiTextureType_BASE_COLOR) == 0) {
        return SdfPath();
    }
    std::string name = PrimNameFor(aiName.C_Str(), "material");
    // unique among the materials written so far
    for (int n = 2;; ++n) {
        bool clash = false;
        for (const SdfPath& path : run.materialPaths) clash = clash || (!path.IsEmpty() && path.GetName() == name);
        if (!clash) break;
        name = PrimNameFor(aiName.C_Str(), "material") + "_" + std::to_string(n);
    }
    if (!run.stage->GetPrimAtPath(run.materialsScope)) UsdGeomScope::Define(run.stage, run.materialsScope);
    const SdfPath path = run.materialsScope.AppendChild(TfToken(name));
    run.writer->Write(path, PreviewOf(run, material, aiName.C_Str()));
    run.materialPaths[index] = path;
    return path;
}

// ------------------------------------------------------------------ meshes

struct PositionKey {
    float x, y, z;
    bool operator==(const PositionKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PositionHash {
    size_t operator()(const PositionKey& k) const {
        uint32_t bits[3];
        std::memcpy(bits, &k, sizeof(bits));
        return (static_cast<size_t>(bits[0]) * 73856093u) ^ (static_cast<size_t>(bits[1]) * 19349663u) ^
               (static_cast<size_t>(bits[2]) * 83492791u);
    }
};

// Per-corner values stored once each, with an index per corner.
template <typename T, typename Hash>
struct Indexed {
    VtArray<T> values;
    VtIntArray indices;
    std::unordered_map<T, int, Hash> known;
    void Add(const T& value) {
        const auto found = known.find(value);
        if (found != known.end()) {
            indices.push_back(found->second);
            return;
        }
        const int index = static_cast<int>(values.size());
        known.emplace(value, index);
        values.push_back(value);
        indices.push_back(index);
    }
};
struct Vec2Hash {
    size_t operator()(const GfVec2f& v) const { return std::hash<float>()(v[0]) * 31 + std::hash<float>()(v[1]); }
};
struct Vec3Hash {
    size_t operator()(const GfVec3f& v) const {
        return (std::hash<float>()(v[0]) * 31 + std::hash<float>()(v[1])) * 31 + std::hash<float>()(v[2]);
    }
};

// The positions of an assimp mesh at `tick`: its blend shapes mixed in,
// then its skin applied, in the space of the node that holds it.
std::vector<aiVector3D> PositionsAt(const Run& run, const aiMesh* mesh, const aiNode* holder, double tick) {
    std::vector<aiVector3D> base(mesh->mVertices, mesh->mVertices + mesh->mNumVertices);
    // blend shapes: the weights the morph channels give at this time
    const auto morph = run.morphs.find(mesh->mName.C_Str());
    if (morph != run.morphs.end() && mesh->mNumAnimMeshes > 0) {
        std::vector<float> weights(mesh->mNumAnimMeshes, 0.0f);
        for (unsigned t = 0; t < mesh->mNumAnimMeshes; ++t) weights[t] = mesh->mAnimMeshes[t]->mWeight;
        for (const aiMeshMorphAnim* channel : morph->second) {
            if (channel->mNumKeys == 0) continue;
            // the key at or before this time, and the next, linearly between
            size_t k = 0;
            while (k + 1 < channel->mNumKeys && channel->mKeys[k + 1].mTime <= tick) ++k;
            const aiMeshMorphKey& a = channel->mKeys[k];
            const aiMeshMorphKey* b = (k + 1 < channel->mNumKeys && tick > a.mTime) ? &channel->mKeys[k + 1] : nullptr;
            const double f = b ? (tick - a.mTime) / (b->mTime - a.mTime) : 0.0;
            std::fill(weights.begin(), weights.end(), 0.0f);
            for (unsigned v = 0; v < a.mNumValuesAndWeights; ++v) {
                if (a.mValues[v] < weights.size()) weights[a.mValues[v]] += static_cast<float>(a.mWeights[v] * (1.0 - f));
            }
            if (b) {
                for (unsigned v = 0; v < b->mNumValuesAndWeights; ++v) {
                    if (b->mValues[v] < weights.size()) weights[b->mValues[v]] += static_cast<float>(b->mWeights[v] * f);
                }
            }
        }
        std::vector<aiVector3D> mixed = base;
        for (unsigned t = 0; t < mesh->mNumAnimMeshes; ++t) {
            const aiAnimMesh* target = mesh->mAnimMeshes[t];
            if (weights[t] == 0.0f || !target->mVertices || target->mNumVertices != mesh->mNumVertices) continue;
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) mixed[v] += (target->mVertices[v] - base[v]) * weights[t];
        }
        base.swap(mixed);
    }
    if (mesh->mNumBones == 0) return base;
    // the skin: each bone's motion since the bind pose, weighted, in the holder's space
    aiMatrix4x4 toHolder = run.GlobalAt(holder, tick);
    toHolder.Inverse();
    std::vector<aiVector3D> out(mesh->mNumVertices, aiVector3D(0, 0, 0));
    std::vector<float> weightSum(mesh->mNumVertices, 0.0f);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone* bone = mesh->mBones[b];
        const auto node = run.nodes.find(bone->mName.C_Str());
        if (node == run.nodes.end()) continue;
        const aiMatrix4x4 skin = toHolder * run.GlobalAt(node->second, tick) * bone->mOffsetMatrix;
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight& weight = bone->mWeights[w];
            if (weight.mVertexId >= mesh->mNumVertices) continue;
            out[weight.mVertexId] += (skin * base[weight.mVertexId]) * weight.mWeight;
            weightSum[weight.mVertexId] += weight.mWeight;
        }
    }
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        if (weightSum[v] <= 0.0f) out[v] = base[v];                 // not on any bone: where it was
        else if (std::fabs(weightSum[v] - 1.0f) > 1e-4f) out[v] /= weightSum[v];  // weights that do not add up to one
    }
    return out;
}

// All the meshes of one node as one USD mesh.
void WriteMesh(Run& run, const aiNode* node, UsdGeomMesh mesh) {
    std::unordered_map<PositionKey, int, PositionHash> weld;
    VtVec3fArray points;
    VtIntArray counts, indices;
    std::vector<Indexed<GfVec2f, Vec2Hash>> uvSets(AI_MAX_NUMBER_OF_TEXTURECOORDS);
    std::vector<bool> uvSetUsed(AI_MAX_NUMBER_OF_TEXTURECOORDS, false);
    Indexed<GfVec3f, Vec3Hash> colours;
    bool hasColours = false;
    VtVec3fArray normals;
    bool allNormals = true;
    std::vector<int> faceMaterial;
    std::vector<std::pair<const aiMesh*, std::vector<int>>> parts;  // each part's vertex -> point
    bool deforms = false;

    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* part = run.scene->mMeshes[node->mMeshes[i]];
        for (unsigned c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) uvSetUsed[c] = uvSetUsed[c] || part->HasTextureCoords(c);
        hasColours = hasColours || part->HasVertexColors(0);
        deforms = deforms || part->mNumBones > 0 || part->mNumAnimMeshes > 0;
    }
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* part = run.scene->mMeshes[node->mMeshes[i]];
        std::vector<int> pointOf(part->mNumVertices);
        for (unsigned v = 0; v < part->mNumVertices; ++v) {
            const PositionKey key{part->mVertices[v].x, part->mVertices[v].y, part->mVertices[v].z};
            const auto found = weld.find(key);
            if (found != weld.end()) {
                pointOf[v] = found->second;
            } else {
                pointOf[v] = static_cast<int>(points.size());
                weld.emplace(key, pointOf[v]);
                points.push_back(GfVec3f(key.x, key.y, key.z));
            }
        }
        allNormals = allNormals && part->HasNormals();
        for (unsigned f = 0; f < part->mNumFaces; ++f) {
            const aiFace& face = part->mFaces[f];
            if (face.mNumIndices < 3) {
                ++run.linesAndPoints;
                continue;
            }
            counts.push_back(static_cast<int>(face.mNumIndices));
            faceMaterial.push_back(static_cast<int>(part->mMaterialIndex));
            for (unsigned k = 0; k < face.mNumIndices; ++k) {
                const unsigned v = face.mIndices[k];
                indices.push_back(pointOf[v]);
                for (unsigned c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
                    if (!uvSetUsed[c]) continue;
                    uvSets[c].Add(part->HasTextureCoords(c)
                                      ? GfVec2f(part->mTextureCoords[c][v].x, part->mTextureCoords[c][v].y)
                                      : GfVec2f(0.0f));
                }
                if (hasColours) {
                    const aiColor4D colour = part->HasVertexColors(0) ? part->mColors[0][v] : aiColor4D(1, 1, 1, 1);
                    colours.Add(GfVec3f(colour.r, colour.g, colour.b));
                }
                if (allNormals) normals.push_back(GfVec3f(part->mNormals[v].x, part->mNormals[v].y, part->mNormals[v].z));
            }
        }
        parts.emplace_back(part, std::move(pointOf));
    }

    mesh.CreateFaceVertexCountsAttr().Set(counts);
    mesh.CreateFaceVertexIndicesAttr().Set(indices);
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);  // polygons as they are
    UsdGeomPrimvarsAPI primvars(mesh);
    for (unsigned c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
        if (!uvSetUsed[c]) continue;
        UsdGeomPrimvar st = primvars.CreatePrimvar(TfToken(c == 0 ? "st" : "st" + std::to_string(c)),
                                                   SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
        st.Set(uvSets[c].values);
        st.SetIndices(uvSets[c].indices);
    }
    if (hasColours) {
        UsdGeomPrimvar colour = mesh.CreateDisplayColorPrimvar(UsdGeomTokens->faceVarying);
        colour.Set(colours.values);
        colour.SetIndices(colours.indices);
    }
    if (allNormals && !deforms && normals.size() == indices.size()) {
        // a deformed mesh would need its normals deformed too: the viewer and Nuke compute them
        mesh.CreateNormalsAttr().Set(normals);
        mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
    }

    // the points: as they are, or deformed frame by frame
    const auto pointsAt = [&](double tick) {
        VtVec3fArray deformed = points;
        for (const auto& part : parts) {
            if (part.first->mNumBones == 0 && part.first->mNumAnimMeshes == 0) continue;
            const std::vector<aiVector3D> moved = PositionsAt(run, part.first, node, tick);
            for (size_t v = 0; v < moved.size(); ++v) {
                deformed[static_cast<size_t>(part.second[v])] = GfVec3f(moved[v].x, moved[v].y, moved[v].z);
            }
        }
        return deformed;
    };
    if (deforms && !run.ticks.empty()) {
        for (const double tick : run.ticks) {
            if (run.progress && run.progress->cancel) {
                run.cancelled = true;
                return;
            }
            const VtVec3fArray frame = pointsAt(tick);
            const UsdTimeCode time(run.FrameOf(tick));
            mesh.CreatePointsAttr().Set(frame, time);
            VtVec3fArray extent;
            if (UsdGeomPointBased::ComputeExtent(frame, &extent)) mesh.CreateExtentAttr().Set(extent, time);
        }
    } else {
        const VtVec3fArray still = deforms ? pointsAt(0.0) : points;
        mesh.CreatePointsAttr().Set(still);
        VtVec3fArray extent;
        if (UsdGeomPointBased::ComputeExtent(still, &extent)) mesh.CreateExtentAttr().Set(extent);
    }
    for (const auto& part : parts) {
        if (part.first->mNumBones > 0) { ++run.skinned; break; }
    }
    for (const auto& part : parts) {
        if (part.first->mNumAnimMeshes > 0) { ++run.morphed; break; }
    }

    // materials: one for the mesh, or one per group of polygons
    std::map<int, VtIntArray> facesOf;
    for (size_t f = 0; f < faceMaterial.size(); ++f) facesOf[faceMaterial[f]].push_back(static_cast<int>(f));
    if (facesOf.size() == 1) {
        const SdfPath material = MaterialFor(run, static_cast<unsigned>(facesOf.begin()->first));
        if (!material.IsEmpty()) {
            UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(UsdShadeMaterial(run.stage->GetPrimAtPath(material)));
        }
    } else if (facesOf.size() > 1) {
        ++run.subsetMeshes;
        std::unordered_map<std::string, int> taken;
        for (const auto& entry : facesOf) {
            const SdfPath material = MaterialFor(run, static_cast<unsigned>(entry.first));
            const std::string name = UniqueName(material.IsEmpty() ? "material" : material.GetName(), &taken);
            UsdGeomSubset subset = UsdGeomSubset::CreateGeomSubset(mesh, TfToken(name), UsdGeomTokens->face, entry.second,
                                                                   UsdShadeTokens->materialBind,
                                                                   UsdGeomTokens->nonOverlapping);
            if (!material.IsEmpty()) {
                UsdShadeMaterialBindingAPI::Apply(subset.GetPrim())
                    .Bind(UsdShadeMaterial(run.stage->GetPrimAtPath(material)));
            }
        }
    }
    ++run.meshes;
    run.polygons += counts.size();
}

// ---------------------------------------------------------- cameras, lights

// A rotation that turns USD's camera / light frame (looking down -Z, Y up)
// to the frame assimp gives (lookAt, up), placed at `position`.
aiMatrix4x4 CameraFrame(const aiVector3D& lookAt, const aiVector3D& up, const aiVector3D& position) {
    aiVector3D forward = lookAt.SquareLength() > 0.0f ? lookAt : aiVector3D(0, 0, -1);
    forward.Normalize();
    aiVector3D upward = up;
    aiVector3D right = forward ^ upward;
    if (right.SquareLength() < 1e-12f) {
        // no up given, or up along the view: any up across it
        upward = std::fabs(forward.y) < 0.9f ? aiVector3D(0, 1, 0) : aiVector3D(1, 0, 0);
        right = forward ^ upward;
    }
    right.Normalize();
    upward = right ^ forward;
    // columns: where local X, Y, Z go (assimp multiplies column vectors)
    return aiMatrix4x4(right.x, upward.x, -forward.x, position.x,
                       right.y, upward.y, -forward.y, position.y,
                       right.z, upward.z, -forward.z, position.z,
                       0, 0, 0, 1);
}

void WriteCamera(const aiCamera* source, UsdGeomCamera camera) {
    // assimp keeps the field of view (half of it: centre to edge), not the
    // film back: a 36 mm wide back and the focal length that gives exactly that view
    const float aspect = source->mAspect > 0.0f ? source->mAspect : 16.0f / 9.0f;
    const float aperture = 36.0f;
    const float fov = source->mHorizontalFOV > 0.0f ? source->mHorizontalFOV * 2.0f : 0.8575f;
    camera.CreateHorizontalApertureAttr().Set(aperture);
    camera.CreateVerticalApertureAttr().Set(aperture / aspect);
    camera.CreateFocalLengthAttr().Set(static_cast<float>(aperture * 0.5 / std::tan(fov * 0.5)));
    camera.CreateClippingRangeAttr().Set(GfVec2f(source->mClipPlaneNear, source->mClipPlaneFar));
    if (source->mOrthographicWidth > 0.0f) {
        camera.CreateProjectionAttr().Set(UsdGeomTokens->orthographic);
        camera.CreateHorizontalApertureAttr().Set(source->mOrthographicWidth * 2.0f * 10.0f);
        camera.CreateVerticalApertureAttr().Set(source->mOrthographicWidth * 2.0f * 10.0f / aspect);
    }
}

UsdPrim WriteLight(Run& run, const aiLight* light, const SdfPath& path) {
    const GfVec3f colour(light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b);
    const float strength = std::max({colour[0], colour[1], colour[2], 1e-6f});
    UsdPrim prim;
    switch (light->mType) {
    case aiLightSource_DIRECTIONAL: prim = UsdLuxDistantLight::Define(run.stage, path).GetPrim(); break;
    case aiLightSource_AREA: {
        UsdLuxRectLight rect = UsdLuxRectLight::Define(run.stage, path);
        rect.CreateWidthAttr().Set(light->mSize.x > 0.0f ? light->mSize.x : 1.0f);
        rect.CreateHeightAttr().Set(light->mSize.y > 0.0f ? light->mSize.y : 1.0f);
        prim = rect.GetPrim();
        break;
    }
    case aiLightSource_AMBIENT: prim = UsdLuxDomeLight::Define(run.stage, path).GetPrim(); break;
    case aiLightSource_SPOT: {
        UsdLuxSphereLight sphere = UsdLuxSphereLight::Define(run.stage, path);
        sphere.CreateTreatAsPointAttr().Set(true);
        UsdLuxShapingAPI shaping = UsdLuxShapingAPI::Apply(sphere.GetPrim());
        shaping.CreateShapingConeAngleAttr().Set(static_cast<float>(light->mAngleOuterCone * 0.5 * 180.0 / 3.14159265358979323846));
        prim = sphere.GetPrim();
        break;
    }
    default: {
        UsdLuxSphereLight sphere = UsdLuxSphereLight::Define(run.stage, path);
        sphere.CreateTreatAsPointAttr().Set(true);
        prim = sphere.GetPrim();
        break;
    }
    }
    UsdLuxLightAPI api(prim);
    api.CreateColorAttr().Set(colour / strength);
    api.CreateIntensityAttr().Set(strength);
    ++run.lightCount;
    return prim;
}

// -------------------------------------------------------------- hierarchy

// Authors the transform of a prim: the folded helpers above it, its own,
// and for a camera or light the turn into USD's frame. Every frame when
// anything in the chain moves.
void WriteTransform(Run& run, UsdGeomXformable xformable, const std::vector<const aiNode*>& chain,
                    const aiMatrix4x4& extra) {
    bool moves = false;
    for (const aiNode* node : chain) moves = moves || run.Moves(node);
    const auto matrixAt = [&](double tick) {
        aiMatrix4x4 m;
        for (const aiNode* node : chain) m = m * run.LocalAt(node, tick);
        return ToUsd(m * extra);
    };
    if (moves && !run.ticks.empty()) {
        UsdGeomXformOp op = xformable.AddTransformOp(UsdGeomXformOp::PrecisionDouble);
        for (const double tick : run.ticks) op.Set(matrixAt(tick), UsdTimeCode(run.FrameOf(tick)));
        ++run.animatedNodes;
        return;
    }
    const GfMatrix4d still = matrixAt(run.ticks.empty() ? 0.0 : run.ticks.front());
    if (still != GfMatrix4d(1.0)) xformable.AddTransformOp(UsdGeomXformOp::PrecisionDouble).Set(still);
}

void Node(Run& run, const aiNode* node, const SdfPath& parentPath, std::vector<const aiNode*> helpers,
          std::unordered_map<std::string, int>* taken) {
    if (run.cancelled) return;
    if (IsHelper(node)) {
        helpers.push_back(node);
        run.Tick();
        for (unsigned i = 0; i < node->mNumChildren; ++i) Node(run, node->mChildren[i], parentPath, helpers, taken);
        return;
    }
    std::vector<const aiNode*> chain = helpers;
    chain.push_back(node);
    const std::string name = node->mName.C_Str();
    const SdfPath path = parentPath.AppendChild(TfToken(UniqueName(PrimNameFor(name, "node"), taken)));
    const auto camera = run.cameras.find(name);
    const auto light = run.lights.find(name);
    UsdGeomXformable xformable;
    if (camera != run.cameras.end()) {
        UsdGeomCamera prim = UsdGeomCamera::Define(run.stage, path);
        WriteCamera(camera->second, prim);
        WriteTransform(run, prim, chain, CameraFrame(camera->second->mLookAt, camera->second->mUp, camera->second->mPosition));
        ++run.cameraCount;
        xformable = prim;
    } else if (light != run.lights.end()) {
        const aiLight* source = light->second;
        UsdPrim prim = WriteLight(run, source, path);
        WriteTransform(run, UsdGeomXformable(prim), chain, CameraFrame(source->mDirection, source->mUp, source->mPosition));
    } else if (node->mNumMeshes > 0 && node->mNumChildren == 0) {
        UsdGeomMesh mesh = UsdGeomMesh::Define(run.stage, path);
        WriteMesh(run, node, mesh);
        WriteTransform(run, mesh, chain, aiMatrix4x4());
    } else {
        UsdGeomXform xform = UsdGeomXform::Define(run.stage, path);
        WriteTransform(run, xform, chain, aiMatrix4x4());
        if (node->mNumMeshes > 0) {
            // a node with geometry and children: the geometry is a child of its own
            std::unordered_map<std::string, int> local;
            for (unsigned i = 0; i < node->mNumChildren; ++i) {
                ++local[PrimNameFor(node->mChildren[i]->mName.C_Str(), "node")];
            }
            WriteMesh(run, node, UsdGeomMesh::Define(run.stage, path.AppendChild(TfToken(UniqueName("geometry", &local)))));
        }
    }
    run.Tick();
    std::unordered_map<std::string, int> childNames;
    for (unsigned i = 0; i < node->mNumChildren; ++i) Node(run, node->mChildren[i], path, {}, &childNames);
}

void Index(Run& run, const aiNode* node) {
    run.nodes[node->mName.C_Str()] = node;
    ++run.nodesTotal;
    for (unsigned i = 0; i < node->mNumChildren; ++i) Index(run, node->mChildren[i]);
}

// FBX's time modes (KTime::EMode) as frames per second.
double FrameRateOf(int mode, double custom) {
    switch (mode) {
    case 1: return 120.0;
    case 2: return 100.0;
    case 3: return 60.0;
    case 4: return 50.0;
    case 5: return 48.0;
    case 6:
    case 7: return 30.0;
    case 8:
    case 9: return 30000.0 / 1001.0;
    case 10: return 25.0;
    case 11: return 24.0;
    case 12: return 1000.0;
    case 13: return 24000.0 / 1001.0;
    case 14: return custom > 0.0 ? custom : 24.0;
    case 15: return 96.0;
    case 16: return 72.0;
    case 17: return 60000.0 / 1001.0;
    default: return 0.0;
    }
}

std::string Plural(size_t n, const std::string& one, const std::string& many) {
    return Count(n) + " " + (n == 1 ? one : many);
}

}  // namespace

Report ImportFbx(const std::string& inputPath, const std::string& usdPath, Progress* progress) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    const fs::path source = PathOf(inputPath);
    if (!IsFile(source)) {
        rep.Fail("cannot read " + inputPath + ": the file is not there");
        return rep;
    }
    if (progress) {
        progress->step = "reading the FBX file";
        progress->fraction = 0.0f;
    }

    Assimp::Importer importer;
    // pivots and offsets folded into the node transforms where assimp can
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // every animation curve, also the ones that hold still
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_OPTIMIZE_EMPTY_ANIMATION_CURVES, false);
    const aiScene* scene = importer.ReadFile(inputPath.c_str(), 0);
    if (!scene || !scene->mRootNode) {
        const std::string why = importer.GetErrorString();
        rep.Fail("cannot read " + inputPath + ": not an FBX file, or a damaged or cut-short one" +
                 (why.empty() ? std::string() : " (" + why + ")"));
        return rep;
    }
    if (progress) progress->fraction = 0.1f;

    Run run;
    run.scene = scene;
    run.rep = &rep;
    run.progress = progress;
    run.source = source;
    run.embeddedDir = PathOf(usdPath).parent_path() / PathOf(Utf8Of(source.stem()) + "_embedded");
    run.stage = UsdStage::CreateInMemory();
    PreviewMaterialWriter writer(run.stage);
    run.writer = &writer;
    run.materialPaths.resize(scene->mNumMaterials);
    for (unsigned i = 0; i < scene->mNumCameras; ++i) run.cameras[scene->mCameras[i]->mName.C_Str()] = scene->mCameras[i];
    for (unsigned i = 0; i < scene->mNumLights; ++i) run.lights[scene->mLights[i]->mName.C_Str()] = scene->mLights[i];
    Index(run, scene->mRootNode);

    // ----- time: the file's frame rate, the frames its first animation covers
    int timeMode = 0;
    float customRate = 0.0f;
    double unitScale = 0.0;
    if (scene->mMetaData) {
        scene->mMetaData->Get("FrameRate", timeMode);
        scene->mMetaData->Get("CustomFrameRate", customRate);
        float unit = 0.0f;
        double unitDouble = 0.0;
        if (scene->mMetaData->Get("UnitScaleFactor", unit)) {
            unitScale = unit;
        } else if (scene->mMetaData->Get("UnitScaleFactor", unitDouble)) {
            unitScale = unitDouble;
        }
    }
    const double fileRate = FrameRateOf(timeMode, customRate);
    std::string otherTakes;
    if (scene->mNumAnimations > 0) {
        const aiAnimation* animation = scene->mAnimations[0];
        for (unsigned i = 1; i < scene->mNumAnimations; ++i) {
            otherTakes += (otherTakes.empty() ? "" : ", ") + std::string(scene->mAnimations[i]->mName.C_Str());
        }
        const double ticksPerSecond = animation->mTicksPerSecond > 0.0 ? animation->mTicksPerSecond
                                                                       : (fileRate > 0.0 ? fileRate : 24.0);
        run.fps = fileRate > 0.0 ? fileRate : ticksPerSecond;
        run.ticksPerFrame = ticksPerSecond / run.fps;
        double startSeconds = 0.0;
        if (FbxAnimationStart(source, &startSeconds)) run.offsetTicks = startSeconds * ticksPerSecond;
        double first = 1e300, last = -1e300;
        const auto span = [&](double t) {
            first = std::min(first, t);
            last = std::max(last, t);
        };
        for (unsigned c = 0; c < animation->mNumChannels; ++c) {
            const aiNodeAnim* channel = animation->mChannels[c];
            run.channels[channel->mNodeName.C_Str()] = channel;
            for (unsigned k = 0; k < channel->mNumPositionKeys; ++k) span(channel->mPositionKeys[k].mTime);
            for (unsigned k = 0; k < channel->mNumRotationKeys; ++k) span(channel->mRotationKeys[k].mTime);
            for (unsigned k = 0; k < channel->mNumScalingKeys; ++k) span(channel->mScalingKeys[k].mTime);
        }
        for (unsigned c = 0; c < animation->mNumMorphMeshChannels; ++c) {
            const aiMeshMorphAnim* channel = animation->mMorphMeshChannels[c];
            std::string mesh = channel->mName.C_Str();
            const size_t star = mesh.find('*');  // assimp names a morph channel "mesh*n"
            if (star != std::string::npos) mesh = mesh.substr(0, star);
            run.morphs[mesh].push_back(channel);
            for (unsigned k = 0; k < channel->mNumKeys; ++k) span(channel->mKeys[k].mTime);
        }
        if (last > first) {
            // every frame from the first key to the last, on the file's frame numbers
            // whole frame numbers of the file, from the first key to the last
            const double firstFrame = std::floor((first + run.offsetTicks) / run.ticksPerFrame + 1e-6);
            const double lastFrame = std::ceil((last + run.offsetTicks) / run.ticksPerFrame - 1e-6);
            for (double f = firstFrame; f <= lastFrame; f += 1.0) {
                run.ticks.push_back(f * run.ticksPerFrame - run.offsetTicks);
            }
        }
    } else if (fileRate > 0.0) {
        run.fps = fileRate;
    }

    // ----- the stage
    UsdGeomSetStageUpAxis(run.stage, UsdGeomTokens->y);  // assimp turns the file to Y up
    if (unitScale > 0.0) UsdGeomSetStageMetersPerUnit(run.stage, unitScale / 100.0);  // FBX counts in centimetres
    const std::string rootName = PrimNameFor(Utf8Of(source.stem()), "fbx");
    UsdGeomXform root = UsdGeomXform::Define(run.stage, SdfPath::AbsoluteRootPath().AppendChild(TfToken(rootName)));
    run.stage->SetDefaultPrim(root.GetPrim());
    run.materialsScope = root.GetPath().AppendChild(TfToken("Materials"));
    MarkImported(run.stage, source);
    // the file's root (with the axis turn assimp puts on it) is our root
    WriteTransform(run, root, {scene->mRootNode}, aiMatrix4x4());
    if (progress) progress->step = "converting the objects";
    std::unordered_map<std::string, int> taken;
    taken["Materials"] = 1;
    for (unsigned i = 0; i < scene->mRootNode->mNumChildren && !run.cancelled; ++i) {
        Node(run, scene->mRootNode->mChildren[i], root.GetPath(), {}, &taken);
    }
    if (scene->mRootNode->mNumMeshes > 0 && !run.cancelled) {
        WriteMesh(run, scene->mRootNode, UsdGeomMesh::Define(run.stage, root.GetPath().AppendChild(TfToken("geometry"))));
    }
    if (run.cancelled) {
        rep.Fail("cancelled");
        return rep;
    }
    run.stage->SetTimeCodesPerSecond(run.fps);
    run.stage->SetFramesPerSecond(run.fps);
    if (!run.ticks.empty()) {
        run.stage->SetStartTimeCode(run.FrameOf(run.ticks.front()));
        run.stage->SetEndTimeCode(run.FrameOf(run.ticks.back()));
    }

    // ----- the report
    size_t materials = 0;
    for (const SdfPath& path : run.materialPaths) materials += path.IsEmpty() ? 0 : 1;
    std::string what = Plural(run.meshes, "mesh", "meshes") + ", " + Count(run.polygons) + " polygons, " +
                       Plural(materials, "material", "materials") + ", " + Count(writer.textures) + " texture(s)";
    if (run.cameraCount) what += ", " + Plural(run.cameraCount, "camera", "cameras");
    if (run.lightCount) what += ", " + Plural(run.lightCount, "light", "lights");
    rep.Info("import", Utf8Of(source.filename()) + ": " + what +
                           (run.ticks.empty() ? std::string(", no animation")
                                              : ", frames " + Number(run.FrameOf(run.ticks.front())) + "-" +
                                                    Number(run.FrameOf(run.ticks.back())) + " at " + Number(run.fps) +
                                                    " fps"));
    if (run.animatedNodes) rep.Info("import", Plural(run.animatedNodes, "object moves", "objects move") + ", sampled every frame");
    if (run.skinned) {
        rep.Info("import", Plural(run.skinned, "skinned mesh", "skinned meshes") +
                               " deformed by their skeleton and written frame by frame as point caches - what Nuke needs");
    }
    if (run.morphed) {
        rep.Info("import", Plural(run.morphed, "mesh", "meshes") + " with blend shapes mixed in, frame by frame");
    }
    if (!otherTakes.empty()) rep.Warn("import", "the file has more animations (takes) - the first one is read; left out: " + otherTakes);
    if (run.subsetMeshes) {
        rep.Info("import", Plural(run.subsetMeshes, "object changes", "objects change") +
                               " material from polygon to polygon - kept as material subsets");
    }
    if (run.embedded) rep.Info("import", Plural(run.embedded, "texture was", "textures were") + " packed inside the .fbx - unpacked");
    if (!run.foundElsewhere.empty()) {
        rep.Info("import", Plural(run.foundElsewhere.size(), "texture", "textures") +
                               " found by name, not where the file says (" + run.foundElsewhere.front() +
                               (run.foundElsewhere.size() > 1 ? ", ...)" : ")"));
    }
    if (!run.missing.empty()) {
        rep.Warn("import", Plural(run.missing.size(), "texture", "textures") + " not found: " + run.missing.front() +
                               (run.missing.size() > 1 ? ", ..." : ""));
    }
    if (!run.heightMaps.empty()) {
        rep.Warn("import", "bump (height) maps left out - the preview material has no input for one (" +
                               run.heightMaps.front() + (run.heightMaps.size() > 1 ? ", ...)" : ")"));
    }
    if (run.cameraCount) {
        rep.Info("import", "camera field of view exact; the film back is not in what the reader gives, a 36 mm wide "
                           "one is assumed and the focal length set to match");
    }
    if (run.lightCount) {
        rep.Info("import", "lights come along with their colour and strength as the file has them (the Nuke export "
                           "leaves lights out unless asked)");
    }
    if (run.linesAndPoints) {
        rep.Warn("import", Count(run.linesAndPoints) + " line(s) and point(s) left out - USDprep reads polygon meshes");
    }

    if (progress) {
        progress->step = "writing the USD file";
        progress->fraction = 0.9f;
    }
    if (!SaveImported(run.stage, usdPath, rep)) return rep;
    rep.after.prims = run.nodesTotal;
    rep.after.meshes = run.meshes;
    rep.after.materials = materials;
    rep.after.cameras = run.cameraCount;
    rep.after.lights = run.lightCount;
    rep.ok = true;
    if (progress) progress->fraction = 1.0f;
    return rep;
}

}  // namespace importer
}  // namespace usdprep

#else  // !USDPREP_WITH_ASSIMP

namespace usdprep {
namespace importer {

Report ImportFbx(const std::string& inputPath, const std::string& usdPath, Progress*) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    rep.Fail("this build of USDprep has no assimp in it (.fbx needs the assimp library at build time)");
    return rep;
}

}  // namespace importer
}  // namespace usdprep

#endif
