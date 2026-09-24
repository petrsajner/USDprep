#include <usdprep/StageInfo.h>

#include <filesystem>

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdLux/lightAPI.h>

namespace usdprep {

namespace {

using namespace pxr;

void CountStage(const UsdStageRefPtr& stage, Report::Counts& counts,
                std::set<std::string>* textureExtensions) {
    const TfToken mesh("Mesh");
    const TfToken material("Material");
    const TfToken shader("Shader");
    const TfToken camera("Camera");

    for (UsdPrim prim :
         UsdPrimRange(stage->GetPseudoRoot(),
                      UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        if (prim.IsPseudoRoot()) continue;
        ++counts.prims;
        if (prim.IsInstanceable()) ++counts.instances;
        // Lights are a family, not a type: SphereLight, DomeLight and a
        // mesh with LightAPI applied all carry UsdLuxLightAPI. Counted
        // independently of the type chain below, so a geometry light shows
        // up as both a mesh and a light — which is what it is.
        if (prim.HasAPI<UsdLuxLightAPI>()) ++counts.lights;

        const TfToken type = prim.GetTypeName();
        if (type == mesh) {
            ++counts.meshes;
        } else if (type == material) {
            ++counts.materials;
        } else if (type == shader) {
            ++counts.shaders;
            // Texture references: asset-valued shader inputs (UsdUVTexture
            // and friends expose their file as inputs:file).
            for (const UsdAttribute& attr : prim.GetAttributes()) {
                if (attr.GetTypeName() != SdfValueTypeNames->Asset) continue;
                if (attr.GetBaseName().GetString().rfind("inputs:", 0) != 0 &&
                    attr.GetName().GetString().rfind("inputs:", 0) != 0) {
                    continue;
                }
                SdfAssetPath value;
                if (attr.Get(&value) && !value.GetAssetPath().empty()) {
                    ++counts.textureRefs;
                    if (textureExtensions) {
                        std::string ext =
                            std::filesystem::path(value.GetAssetPath())
                                .extension()
                                .string();
                        if (!ext.empty()) textureExtensions->insert(ext);
                    }
                }
            }
        } else if (type == camera) {
            ++counts.cameras;
        }
    }
}

}  // namespace

StageInfo InspectStage(const std::string& path, std::string* error) {
    StageInfo info;
    if (error) error->clear();

    std::error_code ec;
    // error codes, not exceptions: a network share that answers with an
    // error must not take the program down
    if (std::filesystem::exists(path, ec)) {
        info.fileSizeBytes = std::filesystem::file_size(path, ec);
    } else if (error) {
        *error = "file does not exist: " + path;
        return info;
    }

    UsdStageRefPtr stage = UsdStage::Open(path, UsdStage::LoadAll);
    if (!stage) {
        if (error) *error = "cannot open stage: " + path;
        return info;
    }

    CountStage(stage, info.counts, &info.meta.textureExtensions);

    TfToken upAxis;
    if (stage->GetMetadata(TfToken("upAxis"), &upAxis)) {
        info.meta.upAxis = upAxis.GetString();
    }
    double d = 0.0;
    if (stage->GetMetadata(TfToken("metersPerUnit"), &d)) info.meta.metersPerUnit = d;
    if (UsdPrim defaultPrim = stage->GetDefaultPrim()) {
        info.meta.defaultPrim = defaultPrim.GetName().GetString();
    }
    info.meta.startTimeCode = stage->GetStartTimeCode();
    info.meta.endTimeCode = stage->GetEndTimeCode();
    info.meta.framesPerSecond = stage->GetFramesPerSecond();
    return info;
}

}  // namespace usdprep
