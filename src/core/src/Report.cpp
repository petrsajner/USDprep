#include <usdprep/Report.h>

#include <cctype>
#include <sstream>

namespace usdprep {

void Report::Info(std::string action, std::string detail) {
    entries.push_back({ReportEntry::Severity::Info, std::move(action), std::move(detail)});
}

void Report::Warn(std::string action, std::string detail) {
    entries.push_back({ReportEntry::Severity::Warning, std::move(action), std::move(detail)});
}

void Report::Fail(std::string error) {
    ok = false;
    this->error = std::move(error);
    entries.push_back({ReportEntry::Severity::Error, "error", this->error});
}

namespace {

std::string FormatSize(uint64_t bytes) {
    std::ostringstream os;
    if (bytes >= 1024 * 1024) {
        os << (double(bytes) / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024) {
        os << (double(bytes) / 1024.0) << " KB";
    } else {
        os << bytes << " B";
    }
    return os.str();
}

void AppendCountLine(std::ostringstream& os, const std::string& label,
                     const Report::Counts& c) {
    os << "  " << label << ": prims " << c.prims << ", meshes " << c.meshes
       << ", materials " << c.materials << ", shaders " << c.shaders
       << ", instances " << c.instances << ", lights " << c.lights
       << ", cameras " << c.cameras << ", texture refs " << c.textureRefs << "\n";
}

std::string SeverityName(ReportEntry::Severity s) {
    switch (s) {
        case ReportEntry::Severity::Info: return "info";
        case ReportEntry::Severity::Warning: return "warning";
        case ReportEntry::Severity::Error: return "error";
    }
    return "?";
}

// Minimal JSON string escaping — enough for paths and messages we emit.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::string CountsToJson(const Report::Counts& c) {
    std::ostringstream os;
    os << "{\"prims\":" << c.prims << ",\"meshes\":" << c.meshes
       << ",\"materials\":" << c.materials << ",\"shaders\":" << c.shaders
       << ",\"instances\":" << c.instances << ",\"lights\":" << c.lights
       << ",\"cameras\":" << c.cameras << ",\"textureRefs\":" << c.textureRefs
       << "}";
    return os.str();
}

}  // namespace

std::string Report::ToText() const {
    std::ostringstream os;
    os << "input:  " << inputPath << "\n";
    if (!outputPath.empty()) {
        os << "output: " << outputPath
           << (outputSizeBytes ? " (" + FormatSize(outputSizeBytes) + ")" : "")
           << "\n";
    }
    if (!error.empty()) {
        os << "error:  " << error << "\n";
    }
    if (!inputPath.empty()) {
        AppendCountLine(os, "before", before);
    }
    if (!outputPath.empty()) {
        AppendCountLine(os, "after ", after);
    }
    for (const auto& e : entries) {
        os << "  [" << SeverityName(e.severity) << "] " << e.action << ": "
           << e.detail << "\n";
    }
    return os.str();
}

std::string Report::ToJson() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    os << "  \"error\": \"" << JsonEscape(error) << "\",\n";
    os << "  \"input\": \"" << JsonEscape(inputPath) << "\",\n";
    os << "  \"output\": \"" << JsonEscape(outputPath) << "\",\n";
    os << "  \"outputSizeBytes\": " << outputSizeBytes << ",\n";
    os << "  \"before\": " << CountsToJson(before) << ",\n";
    os << "  \"after\": " << CountsToJson(after) << ",\n";
    os << "  \"entries\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        os << "    {\"severity\": \"" << SeverityName(e.severity)
           << "\", \"action\": \"" << JsonEscape(e.action)
           << "\", \"detail\": \"" << JsonEscape(e.detail) << "\"}"
           << (i + 1 < entries.size() ? "," : "") << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

}  // namespace usdprep
