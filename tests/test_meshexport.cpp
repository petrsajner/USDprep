// .obj / .abc for Nuke's classic 3D: world space, one file per
// material next to the complete one, and a .nk that wires textures in.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <usdprep/Extract.h>

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

std::string Slurp(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

size_t Count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

bool Reported(const usdprep::Report& rep, const std::string& text) {
    for (const usdprep::ReportEntry& e : rep.entries) {
        if ((e.action == "obj" || e.action == "abc") && e.detail.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main() {
    const fs::path outDir = fs::current_path() / "meshexport_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- several materials, animated points, a still of frame 2 ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "strip.obj").string();
        options.animation = "static";
        options.staticFrame = 2.0;
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/subsets_scene.usda", options);
        CHECK(rep.ok);
        const std::string obj = Slurp(outDir / "strip.obj");
        // world space (the mesh sits at y = 2), at frame 2 (z = 1)
        CHECK(obj.find("\nv 0 2 1\n") != std::string::npos);
        CHECK(Count(obj, "\ng ") == 4);  // Red, Base, Green and the ball without a material
        CHECK(obj.find("\nvt 0.8 0\n") != std::string::npos);  // the green face kept its UVs
        // one file per material, and the script that loads them
        CHECK(fs::exists(outDir / "strip_parts" / "Red.obj"));
        CHECK(fs::exists(outDir / "strip_parts" / "Green.obj"));
        CHECK(Count(Slurp(outDir / "strip_parts" / "Red.obj"), "\nf ") == 1);
        const std::string nk = Slurp(outDir / "strip.nk");
        CHECK(Count(nk, "ReadGeo2 {") == 4);
        CHECK(nk.find("strip_parts/Green.obj") != std::string::npos);
        CHECK(nk.find("Scene {\n inputs 4") != std::string::npos);
        CHECK(nk.find("color {0.05 0.9 0.05 1}") != std::string::npos);  // no texture: the material's colour
        // the .usdc it was made from does not stay behind
        CHECK(!fs::exists(outDir / "strip.usdprep-source.usdc"));
        CHECK(rep.outputSizeBytes == fs::file_size(outDir / "strip.obj"));
        CHECK(Reported(rep, "written in world space at frame 2"));
    }

    // --- a UDIM atlas squeezes UVs in the material; an .obj needs them baked ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root/Panel", "/Root/Looks"};
        options.outputPath = (outDir / "atlas.obj").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/udim_atlas_scene.usda", options);
        CHECK(rep.ok);
        const std::string obj = Slurp(outDir / "atlas.obj");
        CHECK(obj.find("\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n") != std::string::npos);  // was (1,0) .. (3,2)
        const std::string nk = Slurp(outDir / "atlas.nk");
        CHECK(nk.find("atlas_textures") != std::string::npos);
        CHECK(nk.find("grid.atlas.png") != std::string::npos);
        CHECK(nk.find("atlas.obj") != std::string::npos);  // a single material: no parts folder
        CHECK(!fs::exists(outDir / "atlas_parts"));
        CHECK(Reported(rep, "1 with a texture wired in"));
    }

    // --- the UVs the material reads, not the ones called "st"; tiles brought home ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "uvset.obj").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/uvset_scene.usda", options);
        CHECK(rep.ok);
        const std::string obj = Slurp(outDir / "uvset.obj");
        // previewuv, brought home by one tile - and not "st"
        CHECK(obj.find("vt 0.25 0\nvt 0.75 0\nvt 0.75 1\nvt 0.25 1\n") != std::string::npos);
        CHECK(obj.find("vt 0.5 0.5") == std::string::npos);
        CHECK(Reported(rep, "an .abc carries the animation"));
    }

    // --- .abc: the animation goes along, and the .nk lists every object
    //     (a ReadGeo made by a script loads only the first one otherwise) ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "uvset.abc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/uvset_scene.usda", options);
        if (rep.ok) {
            CHECK(fs::file_size(outDir / "uvset.abc") > 0);
            const std::string nk = Slurp(outDir / "uvset.nk");
            CHECK(nk.find("uvset.abc") != std::string::npos);
            CHECK(nk.find("scene_view {{0} imported: 0 1 selected: 0 1 items: /root/Root_Mover/Root_MoverShape "
                          "/root/Root_Still/Root_StillShape}") != std::string::npos);
            CHECK(nk.find("checker.png") != std::string::npos);
            bool range = false;
            for (const usdprep::ReportEntry& e : rep.entries) {
                if (e.action == "abc" && e.detail.find("frames 1-3 at 25 fps") != std::string::npos) range = true;
            }
            CHECK(range);
            CHECK(!fs::exists(outDir / "uvset.usdprep-source.usdc"));

            // one frame asked for: a still, like an .obj
            options.outputPath = (outDir / "uvset_still.abc").string();
            options.animation = "static";
            options.staticFrame = 3.0;
            const usdprep::Report still = usdprep::ExtractPrims(FIXTURE_DIR "/uvset_scene.usda", options);
            CHECK(still.ok);
            CHECK(fs::file_size(outDir / "uvset_still.abc") < fs::file_size(outDir / "uvset.abc"));
        } else {
            // a build without the Alembic library says so instead of writing something else
            CHECK(rep.error.find("no Alembic") != std::string::npos);
            std::printf("note: built without Alembic, .abc checks skipped\n");
        }
    }

    // --- nothing to write is an error, not an empty file ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root/Looks"};
        options.outputPath = (outDir / "empty.obj").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/udim_atlas_scene.usda", options);
        CHECK(!rep.ok);
    }

    if (failures == 0) std::printf("test_meshexport: OK\n");
    return failures == 0 ? 0 : 1;
}
