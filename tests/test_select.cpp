#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <usdprep/Prune.h>
#include <usdprep/Select.h>
#include <usdprep/StageInfo.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace {

bool Contains(const std::vector<std::string>& paths, const std::string& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

usdprep::SelectResult Select(const std::string& scene, usdprep::SelectOptions options) {
    usdprep::SelectResult result = usdprep::SelectPrims(scene, options);
    CHECK(result.error.empty());
    return result;
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/filter_scene.usda";
    const std::string outDir = std::filesystem::current_path().string();

    // --- type filter: concrete schema name ---
    {
        usdprep::SelectOptions options;
        options.types = {"Mesh"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 5);  // Chair, ChairProxy, Table, CameraFrustum, BoltGeo
        CHECK(Contains(r.paths, "/World/Props/Chair"));
        CHECK(Contains(r.paths, "/World/Guides/CameraFrustum"));
        CHECK(r.visited > 4);
    }

    // --- type filter: case-insensitive, and the "light" family covers
    //     both SphereLight and DistantLight ---
    {
        usdprep::SelectOptions options;
        options.types = {"light"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 2);
        CHECK(Contains(r.paths, "/World/Lights/KeyLight"));
        CHECK(Contains(r.paths, "/World/Lights/SunLight"));
    }
    {
        usdprep::SelectOptions options;
        options.types = {"camera"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 1);
        CHECK(r.paths[0] == "/World/ShotCam");
    }

    // --- name filter: plain text means "contains", wildcards mean glob ---
    {
        usdprep::SelectOptions options;
        options.namePattern = "chair";  // case-insensitive substring
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 2);  // Chair + ChairProxy
    }
    {
        usdprep::SelectOptions options;
        options.namePattern = "chair?roxy";  // '?' = exactly one character
        const usdprep::SelectResult glob = Select(scene, options);
        CHECK(glob.paths.size() == 1);
        CHECK(glob.paths[0] == "/World/Props/ChairProxy");

        options.namePattern = "Chair";  // no wildcard: matches both
        const usdprep::SelectResult substring = Select(scene, options);
        CHECK(substring.paths.size() == 2);

        options.namePattern = "*light";  // anchored at the end
        const usdprep::SelectResult suffix = Select(scene, options);
        CHECK(suffix.paths.size() == 2);  // KeyLight, SunLight
    }

    // --- purpose filter: resolved, so the guide group's mesh matches too ---
    {
        usdprep::SelectOptions options;
        options.purposes = {"guide"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 2);  // Guides + CameraFrustum beneath it
        CHECK(Contains(r.paths, "/World/Guides"));
        CHECK(Contains(r.paths, "/World/Guides/CameraFrustum"));

        options.topmostOnly = true;
        const usdprep::SelectResult topmost = Select(scene, options);
        CHECK(topmost.paths.size() == 1);
        CHECK(topmost.paths[0] == "/World/Guides");
    }
    {
        usdprep::SelectOptions options;
        options.purposes = {"proxy", "render"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 2);  // ChairProxy + Table
    }

    // --- roots limit the search; filters are ANDed ---
    {
        usdprep::SelectOptions options;
        options.roots = {"/World/Props"};
        options.types = {"Mesh"};
        const usdprep::SelectResult r = Select(scene, options);
        CHECK(r.paths.size() == 3);
        CHECK(!Contains(r.paths, "/World/Guides/CameraFrustum"));
    }

    // --- error paths ---
    {
        usdprep::SelectOptions options;
        options.roots = {"/World/NoSuchPrim"};
        CHECK(!usdprep::SelectPrims(scene, options).error.empty());
    }
    {
        usdprep::SelectOptions options;
        options.roots = {"World"};  // not absolute
        CHECK(!usdprep::SelectPrims(scene, options).error.empty());
    }

    // --- lights are counted by schema family, not by one concrete type ---
    {
        std::string err;
        const usdprep::StageInfo info = usdprep::InspectStage(scene, &err);
        CHECK(err.empty());
        CHECK(info.counts.lights == 2);
        CHECK(info.counts.meshes == 5);
        CHECK(info.counts.instances == 1);
        CHECK(info.counts.cameras == 1);
    }

    // --- prune by category: drop every light and everything guide-only ---
    {
        usdprep::PruneOptions options;
        options.dropTypes = {"light"};
        options.dropPurposes = {"guide"};
        options.outputPath = outDir + "/prune_by_filter.usdc";
        const usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.lights == 0);
        CHECK(rep.after.meshes == 4);   // CameraFrustum went with the guides
        CHECK(rep.after.cameras == 1);  // the shot camera is not a light
    }

    // --- kept instancing: the filter reaches into the prototypes, once per
    //     prototype, and the instances stay instances ---
    {
        usdprep::PruneOptions options;
        options.dropTypes = {"Mesh"};
        options.deinstance = false;
        options.outputPath = outDir + "/prune_instanced.usdc";
        const usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 0);     // BoltGeo inside the instance went too
        CHECK(rep.after.instances == 1);  // and the instance is still one
        for (const usdprep::ReportEntry& entry : rep.entries) {
            CHECK(entry.detail.find("instanced content") == std::string::npos);
        }
    }

    // --- a filter that matches nothing is a warning, not a failure ---
    {
        usdprep::PruneOptions options;
        options.dropTypes = {"PointInstancer"};
        options.outputPath = outDir + "/prune_no_match.usdc";
        const usdprep::Report rep = usdprep::PruneStage(scene, options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 5);
    }

    if (failures == 0) std::printf("test_select: OK\n");
    return failures == 0 ? 0 : 1;
}
