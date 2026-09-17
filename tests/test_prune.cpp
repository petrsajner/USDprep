#include <cstdio>
#include <filesystem>
#include <string>

#include <usdprep/Prune.h>
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
    const std::string outDir = std::filesystem::current_path().string();

    // --- keep-only: everything except /Root/Cube disappears ---
    {
        usdprep::PruneOptions options;
        options.keepPaths = {"/Root/Cube"};
        options.outputPath = outDir + "/prune_keep.usdc";
        usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 1);

        std::string err;
        usdprep::StageInfo info = usdprep::InspectStage(options.outputPath, &err);
        CHECK(err.empty());
        CHECK(info.counts.meshes == 1);
        CHECK(info.counts.materials == 0);  // Looks scope is outside the mask
    }

    // --- drop-selection: Clutter and Looks are removed, the rest stays ---
    {
        usdprep::PruneOptions options;
        options.dropPaths = {"/Root/Clutter", "/Root/Looks"};
        options.outputPath = outDir + "/prune_drop.usdc";
        usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 2);  // Cube + Sphere
        CHECK(rep.after.materials == 0);
        CHECK(rep.after.shaders == 0);
        CHECK(rep.after.cameras == 1);
    }

    // --- nested drop paths are deduplicated ---
    {
        usdprep::PruneOptions options;
        options.dropPaths = {"/Root/Looks", "/Root/Looks/CubeMaterial"};
        options.outputPath = outDir + "/prune_nested.usdc";
        usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.materials == 0);
        CHECK(rep.after.meshes == 3);
    }

    // --- both modes at once is rejected ---
    {
        usdprep::PruneOptions options;
        options.keepPaths = {"/Root/Cube"};
        options.dropPaths = {"/Root/Sphere"};
        options.outputPath = outDir + "/prune_bad.usdc";
        CHECK(!usdprep::PruneStage(scene, options).ok);
    }

    if (failures == 0) std::printf("test_prune: OK\n");
    return failures == 0 ? 0 : 1;
}
