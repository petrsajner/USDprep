// A long export says how far it is, and stops when asked - leaving
// nothing behind.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <usdprep/Extract.h>
#include <usdprep/Progress.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace fs = std::filesystem;

int main() {
    const fs::path outDir = fs::current_path() / "progress_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);
    const std::string scene = FIXTURE_DIR "/dense_scene.usda";

    // --- to the end: 100 %, and the last step says so ---------------------------
    {
        usdprep::Progress progress;
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "reduced.usdc").string();
        options.simplifyRatio = 0.25;
        options.progress = &progress;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        CHECK(progress.fraction == 1.0f);
        CHECK(std::strcmp(progress.step, "done") == 0);
        CHECK(fs::exists(options.outputPath));
    }

    // --- stopped: an error that says so, no file, no temporary folder -----------
    {
        usdprep::Progress progress;
        progress.cancel = true;
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "stopped.usdc").string();
        options.simplifyRatio = 0.25;
        options.progress = &progress;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(!rep.ok);
        CHECK(rep.error == "cancelled");
        CHECK(!fs::exists(options.outputPath));
        CHECK(!fs::exists(outDir / ".usdprep-tmp-stopped"));
    }

    // --- without a Progress everything runs as before ---------------------------
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "plain.usdc").string();
        CHECK(usdprep::ExtractPrims(scene, options).ok);
    }

    if (failures == 0) std::printf("all progress tests passed\n");
    return failures == 0 ? 0 : 1;
}
