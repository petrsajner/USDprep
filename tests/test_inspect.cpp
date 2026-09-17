#include <cstdio>
#include <filesystem>
#include <string>

#include <usdprep/StageInfo.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main() {
    const std::string scene = FIXTURE_DIR "/simple_scene.usda";

    std::string err;
    usdprep::StageInfo info = usdprep::InspectStage(scene, &err);
    CHECK(err.empty());
    CHECK(info.fileSizeBytes > 0);
    CHECK(info.counts.meshes == 3);       // Cube, Sphere, Chunk
    CHECK(info.counts.materials == 1);    // CubeMaterial
    CHECK(info.counts.shaders == 1);      // PreviewSurface
    CHECK(info.counts.instances == 1);    // Clutter
    CHECK(info.counts.cameras == 1);      // Cam
    CHECK(info.counts.textureRefs == 0);  // no textures in the fixture
    CHECK(info.meta.defaultPrim == "Root");
    CHECK(info.meta.upAxis == "Y");
    CHECK(info.meta.metersPerUnit == 0.01);

    std::string missingErr;
    usdprep::InspectStage(FIXTURE_DIR "/does_not_exist.usda", &missingErr);
    CHECK(!missingErr.empty());

    if (failures == 0) std::printf("test_inspect: OK\n");
    return failures == 0 ? 0 : 1;
}
