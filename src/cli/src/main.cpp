// usdcut — CLI face of usdprep-core.
//
//   usdcut extract <scene.usd(a|c|z)> <prim-path>... -o out.usdz|usdc|usda
//   usdcut prune   <scene> --except /A,/B -o out.usdc   (keep only)
//   usdcut prune   <scene> --drop /A,/B -o out.usdc     (delete selection)
//   usdcut prune   <scene> --drop-type light -o out.usdc
//   usdcut select  <scene> --type Mesh --name "*door*"
//   usdcut inspect <scene> [--report out.json]
//   usdcut presets [<name>]

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <pxr/pxr.h>

#include <usdprep/Extract.h>
#include <usdprep/Prune.h>
#include <usdprep/Recipe.h>
#include <usdprep/Select.h>
#include <usdprep/StageInfo.h>
#include <usdprep/Version.h>

namespace {

std::string UsdVersionString() {
    // OpenUSD's calendar version lives in minor.patch ("25.11"); the major
    // is still 0 and printing it only confuses whoever reports a bug.
    const std::string calendar =
        std::to_string(PXR_MINOR_VERSION) + "." + std::to_string(PXR_PATCH_VERSION);
    return PXR_MAJOR_VERSION == 0
               ? calendar
               : std::to_string(PXR_MAJOR_VERSION) + "." + calendar;
}

void PrintUsage() {
    std::cout
        << "usdcut " << USDPREP_VERSION_STRING
        << " — prepare USD scenes for compositing (USD " << UsdVersionString() << ")\n\n"
        << "usage:\n"
        << "  usdcut extract <scene> <prim-path>... -o <out.usdz|usdc|usda> [options]\n"
        << "  usdcut prune   <scene> (--except <paths> | --drop <paths> |\n"
        << "                          --drop-type <types> | --drop-purpose <purposes>)\n"
        << "                         -o <out> [options]\n"
        << "  usdcut select  <scene> [--type <types>] [--name <pattern>]\n"
        << "                         [--purpose <purposes>] [--under <paths>] [--topmost]\n"
        << "  usdcut inspect <scene> [--report <file.json>]\n"
        << "  usdcut presets [<name>]   list the built-in recipes, or print one\n"
        << "  usdcut version | help\n\n"
        << "common options:\n"
        << "  -o, --output <path>    output file (.usda, .usdc or .usdz)\n"
        << "  --preset <name>        start from a built-in recipe (usdcut presets)\n"
        << "  --recipe <file.json>   start from a recipe file; later flags win\n"
        << "  --report <file.json>   write the operation report as JSON\n"
        << "  --keep-instancing      do not convert instanceable prims to plain prims\n"
        << "  --no-default-prim      do not author defaultPrim on the output\n"
        << "  --no-relink            .usdc/.usda: leave texture paths pointing at\n"
        << "                         the source tree instead of copying the files\n"
        << "                         into a <name>_textures folder next to the output\n"
        << "  --materials <which>    preview | full | all: where an object has a light\n"
        << "                         material for preview and a heavy one for full\n"
        << "                         renders, keep which (default: the recipe's)\n"
        << "  --keep-render-contexts keep outputs:arnold:* and the like, with their shaders\n"
        << "  --keep-unused-materials keep materials nothing binds\n"
        << "  --animation <mode>     all | range | static: keep every time sample, only\n"
        << "                         the shot range, or bake one frame (default: recipe's)\n"
        << "  --frames <a>-<b>       the range for --animation range (default: the\n"
        << "                         scene's own start/end)\n"
        << "  --frame <n>            the frame for --animation static (default: range start)\n\n"
        << "filters (comma-separated lists):\n"
        << "  types                  schema names (Mesh, Camera, SphereLight) or the\n"
        << "                         family name 'light'; case-insensitive\n"
        << "  purposes               default, render, proxy, guide (resolved, so a mesh\n"
        << "                         under a guide group counts as guide)\n"
        << "  name pattern           plain text matches anywhere in the prim name;\n"
        << "                         * and ? turn it into a wildcard match\n";
}

std::vector<std::string> SplitCommaList(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream stream(s);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}

void EmitReport(const usdprep::Report& rep, const std::string& jsonPath) {
    std::cout << rep.ToText();
    if (!jsonPath.empty()) {
        if (!WriteFile(jsonPath, rep.ToJson())) {
            std::cerr << "warning: cannot write JSON report to " << jsonPath << "\n";
        }
    }
}

struct CommonOptions {
    std::string output;
    std::string reportPath;
    std::string preset;
    std::string recipePath;
    bool deinstance = true;
    bool setDefaultPrim = true;
    bool relinkTextures = true;
    // A flag that was actually typed beats whatever the recipe said, so
    // the parser records which ones appeared on the command line.
    bool deinstanceGiven = false;
    bool setDefaultPrimGiven = false;
    bool relinkTexturesGiven = false;
    std::string materials;  // empty = the recipe decides
    bool keepRenderContexts = false;
    bool keepUnusedMaterials = false;
    std::string animation;  // empty = the recipe decides
    double frameStart = usdprep::kStageFrame;
    double frameEnd = usdprep::kStageFrame;
    double staticFrame = usdprep::kStageFrame;
};

// Resolve --preset / --recipe into one recipe. False = already reported.
bool ResolveRecipe(const CommonOptions& common, usdprep::Recipe* recipe) {
    if (!common.preset.empty() && !usdprep::GetPreset(common.preset, recipe)) {
        std::cerr << "usdcut: unknown preset " << common.preset
                  << " (see usdcut presets)\n";
        return false;
    }
    if (!common.recipePath.empty()) {
        std::string error;
        std::vector<std::string> warnings;
        usdprep::Recipe fromFile = *recipe;
        if (!usdprep::LoadRecipe(common.recipePath, &fromFile, &error, &warnings)) {
            std::cerr << "usdcut: " << error << "\n";
            return false;
        }
        for (const std::string& warning : warnings) {
            std::cerr << "warning: " << warning << "\n";
        }
        *recipe = fromFile;
    }
    return true;
}

// Recipe first, then the flags that were typed on top of it.
template <typename Options>
void ApplyCommon(const CommonOptions& common, const usdprep::Recipe& recipe,
                 Options* options) {
    usdprep::ApplyRecipe(recipe, options);
    if (common.deinstanceGiven) options->deinstance = common.deinstance;
    if (common.setDefaultPrimGiven) options->setDefaultPrim = common.setDefaultPrim;
    if (common.relinkTexturesGiven) options->relinkTextures = common.relinkTextures;
    if (!common.materials.empty()) options->materialPurpose = common.materials;
    if (common.keepRenderContexts) options->stripRenderContexts = false;
    if (common.keepUnusedMaterials) options->stripUnusedMaterials = false;
    if (!common.animation.empty()) options->animation = common.animation;
    if (!std::isnan(common.frameStart)) options->frameStart = common.frameStart;
    if (!std::isnan(common.frameEnd)) options->frameEnd = common.frameEnd;
    if (!std::isnan(common.staticFrame)) options->staticFrame = common.staticFrame;
    options->outputPath = common.output;
}

// Returns the index of the first positional argument (input scene), or -1
// on error. Positional prim paths are appended to `positionals`.
int ParseCommon(const std::vector<std::string>& args, size_t start,
                CommonOptions& common, std::vector<std::string>& positionals,
                std::string& error) {
    for (size_t i = start; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-o" || a == "--output") {
            if (++i >= args.size()) { error = "--output needs a value"; return -1; }
            common.output = args[i];
        } else if (a == "--report") {
            if (++i >= args.size()) { error = "--report needs a value"; return -1; }
            common.reportPath = args[i];
        } else if (a == "--preset") {
            if (++i >= args.size()) { error = "--preset needs a value"; return -1; }
            common.preset = args[i];
        } else if (a == "--recipe") {
            if (++i >= args.size()) { error = "--recipe needs a value"; return -1; }
            common.recipePath = args[i];
        } else if (a == "--keep-instancing") {
            common.deinstance = false;
            common.deinstanceGiven = true;
        } else if (a == "--no-default-prim") {
            common.setDefaultPrim = false;
            common.setDefaultPrimGiven = true;
        } else if (a == "--no-relink") {
            common.relinkTextures = false;
            common.relinkTexturesGiven = true;
        } else if (a == "--materials") {
            if (++i >= args.size()) { error = "--materials needs preview, full or all"; return -1; }
            common.materials = args[i];
            if (common.materials != "preview" && common.materials != "full" &&
                common.materials != "all") {
                error = "--materials must be preview, full or all";
                return -1;
            }
        } else if (a == "--keep-render-contexts") {
            common.keepRenderContexts = true;
        } else if (a == "--keep-unused-materials") {
            common.keepUnusedMaterials = true;
        } else if (a == "--animation") {
            if (++i >= args.size()) { error = "--animation needs all, range or static"; return -1; }
            common.animation = args[i];
            if (common.animation != "all" && common.animation != "range" &&
                common.animation != "static") {
                error = "--animation must be all, range or static";
                return -1;
            }
        } else if (a == "--frames") {
            if (++i >= args.size()) { error = "--frames needs <start>-<end>"; return -1; }
            char* rest = nullptr;
            common.frameStart = std::strtod(args[i].c_str(), &rest);
            if (rest == args[i].c_str() || *rest != '-') {
                error = "--frames needs <start>-<end>, e.g. 1001-1050";
                return -1;
            }
            common.frameEnd = std::strtod(rest + 1, nullptr);
            if (common.animation.empty()) common.animation = "range";
        } else if (a == "--frame") {
            if (++i >= args.size()) { error = "--frame needs a frame number"; return -1; }
            common.staticFrame = std::strtod(args[i].c_str(), nullptr);
            if (common.animation.empty()) common.animation = "static";
        } else if (!a.empty() && a[0] == '-') {
            error = "unknown option: " + a;
            return -1;
        } else {
            positionals.push_back(a);
        }
    }
    if (positionals.empty()) { error = "missing input scene"; return -1; }
    return 0;
}

int RunExtract(const std::vector<std::string>& args) {
    CommonOptions common;
    std::vector<std::string> positionals;
    std::string error;
    if (ParseCommon(args, 1, common, positionals, error) != 0 || positionals.size() < 2) {
        std::cerr << "usdcut extract: " << (error.empty() ? "usage: usdcut extract <scene> <prim-path>... -o <out>" : error) << "\n";
        return 2;
    }
    usdprep::Recipe recipe;
    if (!ResolveRecipe(common, &recipe)) return 2;
    usdprep::ExtractOptions options;
    ApplyCommon(common, recipe, &options);
    options.primPaths.assign(positionals.begin() + 1, positionals.end());
    usdprep::Report rep = usdprep::ExtractPrims(positionals[0], options);
    EmitReport(rep, common.reportPath);
    return rep.ok ? 0 : 1;
}

int RunPrune(const std::vector<std::string>& args) {
    CommonOptions common;
    std::vector<std::string> positionals;
    std::string error;
    // --except/--drop are prune-specific and accept comma lists.
    usdprep::PruneOptions options;

    // Prune-specific flags take comma lists; pull them out of the argument
    // vector so the common parser only sees what it knows.
    std::vector<std::string> rest = args;
    for (size_t i = 1; i < rest.size(); /*advanced in loop*/) {
        const std::string& a = rest[i];
        const bool isPruneFlag = a == "--except" || a == "--drop" ||
                                 a == "--drop-type" || a == "--drop-purpose";
        if (!isPruneFlag) {
            ++i;
            continue;
        }
        if (i + 1 >= rest.size()) {
            std::cerr << "usdcut prune: " << a << " needs a comma-separated list\n";
            return 2;
        }
        const std::vector<std::string> values = SplitCommaList(rest[i + 1]);
        if (a == "--except") {
            options.keepPaths = values;
        } else if (a == "--drop") {
            options.dropPaths = values;
        } else if (a == "--drop-type") {
            options.dropTypes = values;
        } else {
            options.dropPurposes = values;
        }
        rest.erase(rest.begin() + i, rest.begin() + i + 2);
    }

    if (ParseCommon(rest, 1, common, positionals, error) != 0 || positionals.size() != 1) {
        std::cerr << "usdcut prune: "
                  << (error.empty() ? "usage: usdcut prune <scene> (--except <paths> | "
                                      "--drop <paths> | --drop-type <types> | "
                                      "--drop-purpose <purposes>) -o <out>"
                                    : error)
                  << "\n";
        return 2;
    }
    usdprep::Recipe recipe;
    if (!ResolveRecipe(common, &recipe)) return 2;
    const std::vector<std::string> typedDropTypes = options.dropTypes;
    const std::vector<std::string> typedDropPurposes = options.dropPurposes;
    ApplyCommon(common, recipe, &options);
    // Categories typed on the command line replace the recipe's, they do
    // not add to them.
    if (!typedDropTypes.empty()) options.dropTypes = typedDropTypes;
    if (!typedDropPurposes.empty()) options.dropPurposes = typedDropPurposes;

    usdprep::Report rep = usdprep::PruneStage(positionals[0], options);
    EmitReport(rep, common.reportPath);
    return rep.ok ? 0 : 1;
}

// `select` prints one prim path per line on stdout (so it pipes into the
// other commands) and the match summary on stderr.
int RunSelect(const std::vector<std::string>& args) {
    usdprep::SelectOptions options;
    std::string scene;
    size_t limit = 0;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool takesValue = a == "--type" || a == "--types" || a == "--name" ||
                                a == "--purpose" || a == "--purposes" ||
                                a == "--under" || a == "--limit";
        if (takesValue && i + 1 >= args.size()) {
            std::cerr << "usdcut select: " << a << " needs a value\n";
            return 2;
        }
        if (a == "--type" || a == "--types") {
            options.types = SplitCommaList(args[++i]);
        } else if (a == "--name") {
            options.namePattern = args[++i];
        } else if (a == "--purpose" || a == "--purposes") {
            options.purposes = SplitCommaList(args[++i]);
        } else if (a == "--under") {
            options.roots = SplitCommaList(args[++i]);
        } else if (a == "--limit") {
            limit = std::strtoul(args[++i].c_str(), nullptr, 10);
        } else if (a == "--topmost") {
            options.topmostOnly = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "usdcut select: unknown option: " << a << "\n";
            return 2;
        } else if (scene.empty()) {
            scene = a;
        } else {
            std::cerr << "usdcut select: only one scene can be searched\n";
            return 2;
        }
    }
    if (scene.empty()) {
        std::cerr << "usdcut select: usage: usdcut select <scene> [--type <types>] "
                     "[--name <pattern>] [--purpose <purposes>] [--under <paths>] "
                     "[--topmost] [--limit <n>]\n";
        return 2;
    }

    const usdprep::SelectResult result = usdprep::SelectPrims(scene, options);
    if (!result.error.empty()) {
        std::cerr << "usdcut select: " << result.error << "\n";
        return 1;
    }
    size_t printed = 0;
    for (const std::string& path : result.paths) {
        if (limit != 0 && printed == limit) break;
        std::cout << path << "\n";
        ++printed;
    }
    std::cerr << result.paths.size() << " match(es) among " << result.visited
              << " prims";
    if (printed < result.paths.size()) {
        std::cerr << " (" << printed << " shown)";
    }
    std::cerr << "\n";
    return 0;
}

// `presets` with no argument lists what ships; with a name it prints that
// recipe as JSON, the starting point for a studio recipe file.
int RunPresets(const std::vector<std::string>& args) {
    usdprep::Recipe recipe;
    if (args.size() > 1) {
        if (!usdprep::GetPreset(args[1], &recipe)) {
            std::cerr << "usdcut presets: unknown preset " << args[1] << "\n";
            return 2;
        }
        std::cout << usdprep::RecipeToJson(recipe);
        return 0;
    }
    for (const std::string& name : usdprep::PresetNames()) {
        usdprep::GetPreset(name, &recipe);
        std::cout << "  " << name << "\n      " << recipe.description << "\n";
    }
    std::cout << "\nusdcut presets <name> prints one as JSON; hand such a file "
                 "to --recipe.\n";
    return 0;
}

int RunInspect(const std::vector<std::string>& args) {
    CommonOptions common;
    std::vector<std::string> positionals;
    std::string error;
    if (ParseCommon(args, 1, common, positionals, error) != 0 || positionals.size() != 1) {
        std::cerr << "usdcut inspect: " << (error.empty() ? "usage: usdcut inspect <scene>" : error) << "\n";
        return 2;
    }
    std::string err;
    usdprep::StageInfo info = usdprep::InspectStage(positionals[0], &err);
    if (!err.empty()) {
        std::cerr << "usdcut inspect: " << err << "\n";
        return 1;
    }
    std::ostringstream os;
    os << "input:    " << positionals[0] << " ("
       << info.fileSizeBytes << " bytes)\n";
    os << "prims:    " << info.counts.prims
       << " (instances " << info.counts.instances << ")\n";
    os << "geometry: meshes " << info.counts.meshes << "\n";
    os << "shading:  materials " << info.counts.materials
       << ", shaders " << info.counts.shaders
       << ", texture refs " << info.counts.textureRefs;
    if (!info.meta.textureExtensions.empty()) {
        os << " [";
        bool first = true;
        for (const std::string& ext : info.meta.textureExtensions) {
            os << (first ? "" : " ") << ext;
            first = false;
        }
        os << "]";
    }
    os << "\n";
    os << "others:   lights " << info.counts.lights
       << ", cameras " << info.counts.cameras << "\n";
    os << "stage:    upAxis " << (info.meta.upAxis.empty() ? "?" : info.meta.upAxis)
       << ", metersPerUnit " << info.meta.metersPerUnit
       << ", defaultPrim " << (info.meta.defaultPrim.empty() ? "-" : info.meta.defaultPrim)
       << ", frames [" << info.meta.startTimeCode << ".." << info.meta.endTimeCode
       << "] @ " << info.meta.framesPerSecond << " fps\n";
    std::cout << os.str();
    if (!common.reportPath.empty()) {
        std::ostringstream js;
        js << "{\n  \"input\": \"" << positionals[0] << "\",\n";
        js << "  \"fileSizeBytes\": " << info.fileSizeBytes << ",\n";
        js << "  \"counts\": {\"prims\":" << info.counts.prims
           << ",\"meshes\":" << info.counts.meshes
           << ",\"materials\":" << info.counts.materials
           << ",\"shaders\":" << info.counts.shaders
           << ",\"instances\":" << info.counts.instances
           << ",\"lights\":" << info.counts.lights
           << ",\"cameras\":" << info.counts.cameras
           << ",\"textureRefs\":" << info.counts.textureRefs << "},\n";
        js << "  \"meta\": {\"upAxis\":\"" << info.meta.upAxis
           << "\",\"metersPerUnit\":" << info.meta.metersPerUnit
           << ",\"defaultPrim\":\"" << info.meta.defaultPrim
           << "\",\"startTimeCode\":" << info.meta.startTimeCode
           << ",\"endTimeCode\":" << info.meta.endTimeCode
           << ",\"framesPerSecond\":" << info.meta.framesPerSecond << "}\n}\n";
        if (!WriteFile(common.reportPath, js.str())) {
            std::cerr << "warning: cannot write JSON report to " << common.reportPath << "\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty()) {
        PrintUsage();
        return 2;
    }
    const std::string& cmd = args[0];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        PrintUsage();
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::cout << "usdcut " << USDPREP_VERSION_STRING
                  << " (usdprep-core " << USDPREP_VERSION_STRING
                  << ", USD " << UsdVersionString() << ")\n";
        return 0;
    }
    if (cmd == "presets") return RunPresets(args);
    if (cmd == "extract") return RunExtract(args);
    if (cmd == "prune") return RunPrune(args);
    if (cmd == "select") return RunSelect(args);
    if (cmd == "inspect") return RunInspect(args);
    std::cerr << "usdcut: unknown command '" << cmd << "' (see 'usdcut help')\n";
    return 2;
}
