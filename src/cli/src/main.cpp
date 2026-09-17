// usdcut — CLI face of usdprep-core.
//
//   usdcut extract <scene.usd(a|c|z)> <prim-path>... -o out.usdz|usdc|usda
//   usdcut prune   <scene> --except /A,/B -o out.usdc   (keep only)
//   usdcut prune   <scene> --drop /A,/B -o out.usdc     (delete selection)
//   usdcut inspect <scene> [--report out.json]

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <pxr/pxr.h>

#include <usdprep/Extract.h>
#include <usdprep/Prune.h>
#include <usdprep/StageInfo.h>
#include <usdprep/Version.h>

namespace {

std::string UsdVersionString() {
    return std::to_string(PXR_MAJOR_VERSION) + "." +
           std::to_string(PXR_MINOR_VERSION) + "." +
           std::to_string(PXR_PATCH_VERSION);
}

void PrintUsage() {
    std::cout
        << "usdcut " << USDPREP_VERSION_STRING
        << " — prepare USD scenes for compositing (USD " << UsdVersionString() << ")\n\n"
        << "usage:\n"
        << "  usdcut extract <scene> <prim-path>... -o <out.usdz|usdc|usda> [options]\n"
        << "  usdcut prune   <scene> (--except <paths> | --drop <paths>) -o <out> [options]\n"
        << "  usdcut inspect <scene> [--report <file.json>]\n"
        << "  usdcut version | help\n\n"
        << "common options:\n"
        << "  -o, --output <path>    output file (.usda, .usdc or .usdz)\n"
        << "  --report <file.json>   write the operation report as JSON\n"
        << "  --keep-instancing      do not convert instanceable prims to plain prims\n"
        << "  --no-default-prim      do not author defaultPrim on the output\n";
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
    bool deinstance = true;
    bool setDefaultPrim = true;
};

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
        } else if (a == "--keep-instancing") {
            common.deinstance = false;
        } else if (a == "--no-default-prim") {
            common.setDefaultPrim = false;
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
    usdprep::ExtractOptions options;
    options.outputPath = common.output;
    options.deinstance = common.deinstance;
    options.setDefaultPrim = common.setDefaultPrim;
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

    std::vector<std::string> rest = args;
    for (size_t i = 2; i < rest.size(); /*advanced in loop*/) {
        const std::string& a = rest[i];
        if (a == "--except" || a == "--drop") {
            if (i + 1 >= rest.size()) {
                std::cerr << "usdcut prune: " << a << " needs a comma-separated path list\n";
                return 2;
            }
            if (a == "--except") {
                options.keepPaths = SplitCommaList(rest[i + 1]);
            } else {
                options.dropPaths = SplitCommaList(rest[i + 1]);
            }
            rest.erase(rest.begin() + i, rest.begin() + i + 2);
        } else {
            ++i;
        }
    }

    if (ParseCommon(rest, 1, common, positionals, error) != 0 || positionals.size() != 1) {
        std::cerr << "usdcut prune: " << (error.empty() ? "usage: usdcut prune <scene> (--except <paths> | --drop <paths>) -o <out>" : error) << "\n";
        return 2;
    }
    options.outputPath = common.output;
    options.deinstance = common.deinstance;
    options.setDefaultPrim = common.setDefaultPrim;

    usdprep::Report rep = usdprep::PruneStage(positionals[0], options);
    EmitReport(rep, common.reportPath);
    return rep.ok ? 0 : 1;
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
    if (cmd == "extract") return RunExtract(args);
    if (cmd == "prune") return RunPrune(args);
    if (cmd == "inspect") return RunInspect(args);
    std::cerr << "usdcut: unknown command '" << cmd << "' (see 'usdcut help')\n";
    return 2;
}
