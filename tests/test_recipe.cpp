// A recipe is what a run decides; presets are the recipes we ship.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <usdprep/Extract.h>
#include <usdprep/Recipe.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string WriteTempRecipe(const fs::path& dir, const std::string& name,
                            const std::string& content) {
    const fs::path path = dir / name;
    std::ofstream file(path, std::ios::binary);
    file << content;
    return path.string();
}

}  // namespace

int main() {
    const fs::path outDir = fs::current_path() / "recipe_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- the shipped presets ---
    {
        const std::vector<std::string> names = usdprep::PresetNames();
        CHECK(Contains(names, "nuke"));
        CHECK(Contains(names, "raw"));

        usdprep::Recipe nuke;
        CHECK(usdprep::GetPreset("nuke", &nuke));
        CHECK(nuke.deinstance);
        CHECK(nuke.relinkTextures);
        CHECK(Contains(nuke.dropPurposes, "guide"));
        CHECK(Contains(nuke.dropPurposes, "proxy"));
        CHECK(!nuke.description.empty());

        usdprep::Recipe raw;
        CHECK(usdprep::GetPreset("raw", &raw));
        CHECK(!raw.deinstance);
        CHECK(!raw.relinkTextures);
        CHECK(raw.dropPurposes.empty());

        usdprep::Recipe unknown;
        CHECK(!usdprep::GetPreset("nuke2", &unknown));
    }

    // --- what we print is what we can read back ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        const std::string path = WriteTempRecipe(outDir, "roundtrip.json",
                                                 usdprep::RecipeToJson(nuke));
        usdprep::Recipe loaded;
        std::string error;
        CHECK(usdprep::LoadRecipe(path, &loaded, &error));
        CHECK(error.empty());
        CHECK(loaded.name == nuke.name);
        CHECK(loaded.description == nuke.description);
        CHECK(loaded.deinstance == nuke.deinstance);
        CHECK(loaded.setDefaultPrim == nuke.setDefaultPrim);
        CHECK(loaded.relinkTextures == nuke.relinkTextures);
        CHECK(loaded.dropPurposes == nuke.dropPurposes);
        CHECK(loaded.dropTypes == nuke.dropTypes);
    }

    // --- a studio recipe is a preset plus the two things it changes ---
    {
        const std::string path = WriteTempRecipe(
            outDir, "studio.json",
            "{\"name\": \"studio\", \"preset\": \"nuke\", \"relinkTextures\": false,"
            " \"dropTypes\": [\"light\"]}");
        usdprep::Recipe loaded;
        std::string error;
        CHECK(usdprep::LoadRecipe(path, &loaded, &error));
        CHECK(loaded.name == "studio");
        CHECK(loaded.deinstance);                        // inherited from nuke
        CHECK(Contains(loaded.dropPurposes, "guide"));   // inherited from nuke
        CHECK(!loaded.relinkTextures);                   // overridden
        CHECK(loaded.dropTypes == std::vector<std::string>{"light"});
    }

    // --- a key from a later version is a warning, not a dead end ---
    {
        const std::string path = WriteTempRecipe(
            outDir, "future.json",
            "{\"deinstance\": false, \"decimateRatio\": 0.5}");
        usdprep::Recipe loaded;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(usdprep::LoadRecipe(path, &loaded, &error, &warnings));
        CHECK(!loaded.deinstance);
        CHECK(warnings.size() == 1);
        CHECK(warnings[0].find("decimateRatio") != std::string::npos);
    }

    // --- broken recipes say what is wrong ---
    {
        usdprep::Recipe loaded;
        std::string error;
        CHECK(!usdprep::LoadRecipe((outDir / "no_such_file.json").string(), &loaded,
                                   &error));
        CHECK(!error.empty());

        const std::string broken =
            WriteTempRecipe(outDir, "broken.json", "{\"deinstance\": tru");
        CHECK(!usdprep::LoadRecipe(broken, &loaded, &error));
        CHECK(!error.empty());

        const std::string wrongType =
            WriteTempRecipe(outDir, "wrong_type.json", "{\"deinstance\": \"yes\"}");
        CHECK(!usdprep::LoadRecipe(wrongType, &loaded, &error));
        CHECK(error.find("deinstance") != std::string::npos);

        const std::string wrongList =
            WriteTempRecipe(outDir, "wrong_list.json", "{\"dropTypes\": \"light\"}");
        CHECK(!usdprep::LoadRecipe(wrongList, &loaded, &error));
        CHECK(!error.empty());
    }

    // --- the nuke preset, applied: guide and proxy geometry is gone ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        usdprep::ExtractOptions options;
        usdprep::ApplyRecipe(nuke, &options);
        options.primPaths = {"/World"};
        options.outputPath = (outDir / "world_nuke.usdc").string();
        const usdprep::Report rep =
            usdprep::ExtractPrims(FIXTURE_DIR "/filter_scene.usda", options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 3);   // Chair, Table, the de-instanced BoltGeo
        CHECK(rep.after.instances == 0);
        CHECK(rep.after.lights == 2);   // lights and the camera are kept
        CHECK(rep.after.cameras == 1);
    }

    // --- the raw preset keeps everything as it was ---
    {
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        usdprep::ExtractOptions options;
        usdprep::ApplyRecipe(raw, &options);
        options.primPaths = {"/World"};
        options.outputPath = (outDir / "world_raw.usdc").string();
        const usdprep::Report rep =
            usdprep::ExtractPrims(FIXTURE_DIR "/filter_scene.usda", options);
        CHECK(rep.ok);
        CHECK(rep.after.meshes == 5);
        CHECK(rep.after.instances == 1);
    }

    if (failures == 0) std::printf("test_recipe: OK\n");
    return failures == 0 ? 0 : 1;
}
