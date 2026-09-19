// Golden-file tests: the same input plus the same recipe must produce the
// same file, run after run and release after release.
//
// Two things are checked. Determinism (two runs, byte for byte) is always
// true regardless of the USD version. The golden comparison is against
// committed, path-normalized .usda output and its report; a USD upgrade
// can legitimately change formatting there, so regenerate with
//   USDPREP_UPDATE_GOLDEN=1 ctest -R test_golden
// and read the diff before committing it.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <usdprep/Extract.h>
#include <usdprep/Prune.h>
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

std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
}

void ReplaceAll(std::string* text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = text->find(from); at != std::string::npos;
         at = text->find(from, at + to.size())) {
        text->replace(at, from.size(), to);
    }
}

// Everything that legitimately differs between two machines: path
// separators, where the fixture and the output live, and the output size
// (which the report prints with the machine's own float formatting).
std::string Normalize(std::string text, const std::string& fixtureDir,
                      const std::string& outDir) {
    ReplaceAll(&text, "\r\n", "\n");
    ReplaceAll(&text, "\\", "/");
    std::string fixtures = fixtureDir;
    std::string out = outDir;
    ReplaceAll(&fixtures, "\\", "/");
    ReplaceAll(&out, "\\", "/");
    ReplaceAll(&text, out, "<OUT>");
    ReplaceAll(&text, fixtures, "<FIXTURES>");

    std::ostringstream result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        // "output: <path> (12.34 KB)" — the size is not the point here.
        if (line.rfind("output: ", 0) == 0) {
            const size_t paren = line.rfind(" (");
            if (paren != std::string::npos) line = line.substr(0, paren) + " (<size>)";
        }
        result << line << "\n";
    }
    return result.str();
}

bool UpdatingGoldens() {
    const char* value = std::getenv("USDPREP_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && std::string(value) != "0";
}

// Compare against the committed golden, or rewrite it on request.
void CheckGolden(const std::string& goldenPath, const std::string& actual) {
    if (UpdatingGoldens()) {
        WriteFile(goldenPath, actual);
        std::printf("updated golden %s\n", goldenPath.c_str());
        return;
    }
    if (!fs::exists(goldenPath)) {
        std::printf("FAIL missing golden %s (run with USDPREP_UPDATE_GOLDEN=1)\n",
                    goldenPath.c_str());
        ++failures;
        return;
    }
    std::string expected = ReadFile(goldenPath);
    ReplaceAll(&expected, "\r\n", "\n");
    if (expected == actual) return;

    ++failures;
    std::printf("FAIL golden mismatch: %s\n", goldenPath.c_str());
    // Point at the first line that differs — a full diff belongs in the
    // developer's own diff tool, the path to both files is right here.
    std::istringstream expectedLines(expected);
    std::istringstream actualLines(actual);
    std::string a;
    std::string b;
    int lineNumber = 1;
    while (true) {
        const bool hasExpected = static_cast<bool>(std::getline(expectedLines, a));
        const bool hasActual = static_cast<bool>(std::getline(actualLines, b));
        if (!hasExpected && !hasActual) break;
        if (!hasExpected) a.clear();
        if (!hasActual) b.clear();
        if (a != b) {
            std::printf("  first difference on line %d\n    golden: %s\n    now:    %s\n",
                        lineNumber, a.c_str(), b.c_str());
            break;
        }
        ++lineNumber;
    }
}

}  // namespace

int main() {
    const std::string fixtureDir = FIXTURE_DIR;
    const std::string goldenDir = std::string(FIXTURE_DIR) + "/golden";
    const fs::path outDir = fs::current_path() / "golden_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);
    fs::create_directories(goldenDir);

    usdprep::Recipe nuke;
    usdprep::GetPreset("nuke", &nuke);
    usdprep::Recipe raw;
    usdprep::GetPreset("raw", &raw);

    // --- extract with the nuke preset ---
    {
        usdprep::ExtractOptions options;
        usdprep::ApplyRecipe(nuke, &options);
        options.primPaths = {"/World"};
        options.outputPath = (outDir / "extract_nuke.usda").string();
        const usdprep::Report rep =
            usdprep::ExtractPrims(fixtureDir + "/filter_scene.usda", options);
        CHECK(rep.ok);

        CheckGolden(goldenDir + "/extract_nuke.usda",
                    Normalize(ReadFile(options.outputPath), fixtureDir, outDir.string()));
        CheckGolden(goldenDir + "/extract_nuke.report.txt",
                    Normalize(rep.ToText(), fixtureDir, outDir.string()));

        // Same recipe, same input, second run: identical bytes.
        usdprep::ExtractOptions again = options;
        again.outputPath = (outDir / "extract_nuke_again.usda").string();
        CHECK(usdprep::ExtractPrims(fixtureDir + "/filter_scene.usda", again).ok);
        CHECK(ReadFile(options.outputPath) == ReadFile(again.outputPath));
    }

    // --- prune with the raw preset, and the binary format ---
    {
        usdprep::PruneOptions options;
        usdprep::ApplyRecipe(raw, &options);
        options.dropPaths = {"/World/Lights"};
        options.outputPath = (outDir / "prune_raw.usda").string();
        const usdprep::Report rep =
            usdprep::PruneStage(fixtureDir + "/filter_scene.usda", options);
        CHECK(rep.ok);

        CheckGolden(goldenDir + "/prune_raw.usda",
                    Normalize(ReadFile(options.outputPath), fixtureDir, outDir.string()));
        CheckGolden(goldenDir + "/prune_raw.report.txt",
                    Normalize(rep.ToText(), fixtureDir, outDir.string()));
    }

    // --- crate output is deterministic too (it is what ships) ---
    {
        // Same file name in two folders: the relinked texture paths carry
        // the output's own name, so a differently named run differs by
        // design and would say nothing about determinism.
        fs::create_directories(outDir / "run_a");
        fs::create_directories(outDir / "run_b");
        usdprep::ExtractOptions first;
        usdprep::ApplyRecipe(nuke, &first);
        first.primPaths = {"/Root"};
        first.outputPath = (outDir / "run_a" / "textured.usdc").string();
        usdprep::ExtractOptions second = first;
        second.outputPath = (outDir / "run_b" / "textured.usdc").string();
        const std::string scene = fixtureDir + "/textured_scene.usda";
        CHECK(usdprep::ExtractPrims(scene, first).ok);
        CHECK(usdprep::ExtractPrims(scene, second).ok);
        const std::string a = ReadFile(first.outputPath);
        const std::string b = ReadFile(second.outputPath);
        CHECK(!a.empty());
        CHECK(a == b);
    }

    if (failures == 0) std::printf("test_golden: OK\n");
    return failures == 0 ? 0 : 1;
}
