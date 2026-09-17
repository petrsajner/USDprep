#include <cstdio>
#include <filesystem>
#include <string>

#include <usdprep/Extract.h>
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
    const std::string outDir = [] {
        std::filesystem::path p = std::filesystem::current_path();
        return p.string();
    }();

    // --- whole-tree extraction: de-instancing must kick in ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = outDir + "/extract_root.usdc";
        usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 3);
        CHECK(rep.after.instances == 0);  // Clutter de-instanced
        CHECK(rep.after.materials == 1);
        CHECK(std::filesystem::exists(options.outputPath));

        std::string err;
        usdprep::StageInfo info = usdprep::InspectStage(options.outputPath, &err);
        CHECK(err.empty());
        CHECK(info.counts.meshes == 3);
        CHECK(info.counts.instances == 0);
        CHECK(info.meta.defaultPrim == "Root");
    }

    // --- single-prop extraction with usdz packaging ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root/Cube"};
        options.outputPath = outDir + "/extract_cube.usdz";
        usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 1);
        CHECK(std::filesystem::exists(options.outputPath));
        CHECK(rep.outputSizeBytes > 0);

        std::string err;
        usdprep::StageInfo info = usdprep::InspectStage(options.outputPath, &err);
        CHECK(err.empty());
        CHECK(info.counts.meshes == 1);
    }

    // --- error paths ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root/DoesNotExist"};
        options.outputPath = outDir + "/extract_bad.usdc";
        usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(!rep.ok);
        CHECK(!rep.error.empty());
    }
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"Root"};  // not absolute
        options.outputPath = outDir + "/extract_bad2.usdc";
        CHECK(!usdprep::ExtractPrims(scene, options).ok);
    }

    if (failures == 0) std::printf("test_extract: OK\n");
    return failures == 0 ? 0 : 1;
}
