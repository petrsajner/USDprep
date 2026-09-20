// The texture cap: scaled copies in the output, originals untouched.
#include <cstdint>
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

// Width and height straight from the PNG header (IHDR follows the
// 8-byte signature and the chunk length/type: bytes 16..23).
bool PngSize(const fs::path& path, int* width, int* height) {
    std::ifstream file(path, std::ios::binary);
    unsigned char header[24];
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    const auto be32 = [&](int at) {
        return (static_cast<uint32_t>(header[at]) << 24) | (static_cast<uint32_t>(header[at + 1]) << 16) |
               (static_cast<uint32_t>(header[at + 2]) << 8) | static_cast<uint32_t>(header[at + 3]);
    };
    *width = static_cast<int>(be32(16));
    *height = static_cast<int>(be32(20));
    return true;
}

// The one file with that name anywhere under `dir`.
fs::path Find(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() == name) return entry.path();
    }
    return {};
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/bigtex_scene.usda";
    const fs::path outDir = fs::current_path() / "textures_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- cap at 64: the big texture and both tiles shrink, the 1x1 one
    //     is left alone, and the set stays a set ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "capped.usdc").string();
        options.maxTextureSize = 64;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        const fs::path sidecar = outDir / "capped_textures";
        int w = 0, h = 0;
        CHECK(PngSize(Find(sidecar, "big.png"), &w, &h) && w == 64 && h == 32);
        CHECK(PngSize(Find(sidecar, "big.1001.png"), &w, &h) && w == 64 && h == 32);
        CHECK(PngSize(Find(sidecar, "big.1002.png"), &w, &h) && w == 64 && h == 32);
        CHECK(PngSize(Find(sidecar, "checker.png"), &w, &h) && w == 1 && h == 1);
        bool reported = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "textures" && e.detail.find("3 texture file(s) larger than 64") != std::string::npos) {
                reported = true;
            }
        }
        CHECK(reported);
        // the originals are what they were
        CHECK(PngSize(FIXTURE_DIR "/textures/big.png", &w, &h) && w == 256 && h == 128);
    }

    // --- no cap: copies are the originals ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "uncapped.usdc").string();
        options.maxTextureSize = 0;
        CHECK(usdprep::ExtractPrims(scene, options).ok);
        int w = 0, h = 0;
        CHECK(PngSize(Find(outDir / "uncapped_textures", "big.png"), &w, &h) && w == 256 && h == 128);
    }

    // --- a cap above every texture changes nothing ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "above.usdc").string();
        options.maxTextureSize = 4096;
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        for (const usdprep::ReportEntry& e : rep.entries) {
            CHECK(e.detail.find("scaled down") == std::string::npos);
        }
    }

    // --- the package takes the capped copies too ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "capped.usdz").string();
        options.maxTextureSize = 64;
        CHECK(usdprep::ExtractPrims(scene, options).ok);
        std::error_code ec;
        const auto packageSize = fs::file_size(options.outputPath, ec);
        CHECK(packageSize > 0 && packageSize < 60 * 1024);  // the originals alone are ~10 KB more
    }

    // --- a single-tile UDIM set becomes the tile: Nuke reads that ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "solo.usdc").string();
        const usdprep::Report rep = usdprep::ExtractPrims(FIXTURE_DIR "/single_udim_scene.usda", options);
        CHECK(rep.ok);
        bool collapsed = false;
        bool udimWarning = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.detail.find("single-tile UDIM") != std::string::npos) collapsed = true;
            if (e.action == "nuke") udimWarning = true;
        }
        CHECK(collapsed);
        CHECK(!udimWarning);
        CHECK(!Find(outDir / "solo_textures", "solo.1001.png").empty());
    }

    // --- a multi-tile set and a .usdz are said out loud ---
    {
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "warned.usdz").string();
        const usdprep::Report rep = usdprep::ExtractPrims(scene, options);
        CHECK(rep.ok);
        int nukeWarnings = 0;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "nuke") ++nukeWarnings;
        }
        CHECK(nukeWarnings == 2);  // the UDIM set and the package
    }

    // --- the presets ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.maxTextureSize == 4096);
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(raw.maxTextureSize == 0);
    }

    if (failures == 0) std::printf("test_textures: OK\n");
    return failures == 0 ? 0 : 1;
}
