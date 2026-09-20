// The panel's output-path rules. These are the lines that once produced a
// file called ".usdz.usdz", so they are worth pinning down.
#include <cstdio>
#include <filesystem>
#include <string>

#include "../src/addon/UsdPrep/OutputPath.h"

static int failures = 0;

static void Expect(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}

int main() {
    using namespace usdprep_addon;

    // --- naming the file ---
    Expect(WithExtension(".usdz", ".usdz"), ".usdz", "a bare extension is not doubled");
    Expect(WithExtension("car", ".usdz"), "car.usdz", "a name gains the extension");
    Expect(WithExtension("car.usdc", ".usdz"), "car.usdz", "the extension is swapped");
    Expect(WithExtension("C:/out/car.usda", ".usdc"), "C:/out/car.usdc",
           "the folder survives the swap");

    Expect(OutputNameOf(".usdz"), "", "a bare extension is not a name");
    Expect(OutputNameOf("car.usdz"), "car", "name without the extension");
    Expect(OutputNameOf("C:/out/car.usdz"), "car", "name without folder or extension");
    Expect(OutputNameOf("C:/out/"), "", "a folder is not a file name");
    Expect(OutputNameOf(""), "", "an empty path has no name");
    Expect(OutputNameOf("car"), "car", "a name without an extension is still a name");

    // --- what stops the export ---
    Expect(ExportBlocker(false, "C:/out/car.usdz"),
           "Check at least one object above, or click one in the 3D view.",
           "nothing selected");
    Expect(ExportBlocker(true, ".usdz"), "Give the exported file a name.",
           "output is only an extension");
    Expect(ExportBlocker(true, ""), "Give the exported file a name.", "no output at all");
    Expect(ExportBlocker(true, "C:/out/"), "Give the exported file a name.",
           "output is a folder");
    Expect(ExportBlocker(true, "car.usdz"), "", "a bare file name is fine");
    Expect(ExportBlocker(true, "/car.usdz"), "", "a rooted path is fine");
    Expect(ExportBlocker(true, "C:/definitely/not/here/car.usdz"),
           "There is no folder 'C:/definitely/not/here'.", "the folder does not exist");
    {
        const std::string here = std::filesystem::current_path().string() + "/car.usdz";
        Expect(ExportBlocker(true, here), "", "an existing folder is fine");
    }

    // --- the classic-3D format switches the extension both ways ---
    Expect(WithExtension("C:/out/car.usdc", ".obj"), "C:/out/car.obj", "usdc becomes obj");
    Expect(WithExtension("C:/out/car.obj", ".usdc"), "C:/out/car.usdc", "obj becomes usdc");
    Expect(WithExtension(".obj", ".obj"), ".obj", "a bare .obj is not doubled");
    Expect(OutputNameOf("C:/out/car.obj"), "car", "the name of an .obj");

    if (failures == 0) std::printf("test_outputpath: OK\n");
    return failures == 0 ? 0 : 1;
}
