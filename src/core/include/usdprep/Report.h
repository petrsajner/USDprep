#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usdprep {

// One line in the operation log: what the tool did (or refused to do).
struct ReportEntry {
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Info;
    std::string action;  // short verb: "flatten", "de-instance", "package", ...
    std::string detail;
};

// Result of a usdprep operation: outcome, log, and before/after numbers.
// "before" describes the source stage as opened; "after" the emitted output.
struct Report {
    struct Counts {
        size_t prims = 0;
        size_t meshes = 0;
        size_t materials = 0;
        size_t shaders = 0;
        size_t instances = 0;
        size_t lights = 0;
        size_t cameras = 0;
        size_t textureRefs = 0;
    };

    bool ok = false;
    std::string error;
    std::vector<ReportEntry> entries;

    std::string inputPath;
    std::string outputPath;
    uint64_t outputSizeBytes = 0;

    Counts before;
    Counts after;

    void Info(std::string action, std::string detail);
    void Warn(std::string action, std::string detail);
    void Fail(std::string error);

    std::string ToText() const;
    std::string ToJson() const;
};

}  // namespace usdprep
