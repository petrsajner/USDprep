// Where an FBX's animation starts, read from the file itself.
//
// assimp moves every key of an animation so that it starts at 0: at the
// stack's LocalStart, or - when the stack has none - at its first key. A
// camera track that starts at frame 1001 would come in at frame 0, and a
// comp lines plates up by frame numbers. So the start is read here: the
// first AnimationStack's LocalStart, else the earliest KeyTime of any
// curve. Binary FBX (the node records of the format, zlib-packed arrays)
// and ASCII FBX both.

#include "FbxScan.h"

#ifdef USDPREP_WITH_ASSIMP  // zlib comes with it

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <zlib.h>

namespace usdprep {
namespace importer {

namespace {

constexpr double kTicksPerSecond = 46186158000.0;  // FBX time: KTime units per second

// ------------------------------------------------------------------ binary

struct Binary {
    const std::vector<char>& data;
    bool wide = false;  // 7500 and later: 64-bit offsets
    bool ok = true;

    template <typename T>
    T At(size_t offset) const {
        T value{};
        if (offset + sizeof(T) <= data.size()) std::memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }
};

struct Record {
    size_t end = 0;          // offset after the record
    size_t properties = 0;   // offset of its first property
    uint64_t count = 0;      // number of properties
    size_t children = 0;     // offset of its first child record
    std::string name;
    bool null = true;
};

Record ReadRecord(const Binary& file, size_t offset) {
    Record record;
    const size_t head = file.wide ? 25 : 13;
    if (offset + head > file.data.size()) return record;
    const uint64_t end = file.wide ? file.At<uint64_t>(offset) : file.At<uint32_t>(offset);
    record.count = file.wide ? file.At<uint64_t>(offset + 8) : file.At<uint32_t>(offset + 4);
    const uint64_t listLength = file.wide ? file.At<uint64_t>(offset + 16) : file.At<uint32_t>(offset + 8);
    const uint8_t nameLength = file.At<uint8_t>(offset + head - 1);
    if (end == 0) return record;  // the null record that closes a list
    record.null = false;
    record.end = static_cast<size_t>(end);
    record.name.assign(file.data.data() + offset + head, nameLength);
    record.properties = offset + head + nameLength;
    record.children = record.properties + static_cast<size_t>(listLength);
    return record;
}

// The size of one property, so that the next can be found.
size_t PropertySize(const Binary& file, size_t offset) {
    switch (file.At<char>(offset)) {
    case 'Y': return 1 + 2;
    case 'C': return 1 + 1;
    case 'I':
    case 'F': return 1 + 4;
    case 'D':
    case 'L': return 1 + 8;
    case 'S':
    case 'R': return 1 + 4 + file.At<uint32_t>(offset + 1);
    case 'f':
    case 'd':
    case 'l':
    case 'i':
    case 'b': return 1 + 12 + file.At<uint32_t>(offset + 9);
    default: return 0;  // not FBX as we know it
    }
}

std::string StringProperty(const Binary& file, size_t offset) {
    if (file.At<char>(offset) != 'S') return {};
    const uint32_t length = file.At<uint32_t>(offset + 1);
    if (offset + 5 + length > file.data.size()) return {};
    return std::string(file.data.data() + offset + 5, length);
}

bool NumberProperty(const Binary& file, size_t offset, double* value) {
    switch (file.At<char>(offset)) {
    case 'L': *value = static_cast<double>(file.At<int64_t>(offset + 1)); return true;
    case 'I': *value = file.At<int32_t>(offset + 1); return true;
    case 'D': *value = file.At<double>(offset + 1); return true;
    case 'F': *value = file.At<float>(offset + 1); return true;
    default: return false;
    }
}

// An int64 array property ('l'), unpacked.
bool Int64Array(const Binary& file, size_t offset, std::vector<int64_t>* out) {
    if (file.At<char>(offset) != 'l') return false;
    const uint32_t length = file.At<uint32_t>(offset + 1);
    const uint32_t encoding = file.At<uint32_t>(offset + 5);
    const uint32_t packed = file.At<uint32_t>(offset + 9);
    const size_t start = offset + 13;
    if (start + packed > file.data.size()) return false;
    out->assign(length, 0);
    if (encoding == 0) {
        if (packed < length * sizeof(int64_t)) return false;
        std::memcpy(out->data(), file.data.data() + start, length * sizeof(int64_t));
        return true;
    }
    uLongf size = static_cast<uLongf>(length * sizeof(int64_t));
    return uncompress(reinterpret_cast<Bytef*>(out->data()), &size,
                      reinterpret_cast<const Bytef*>(file.data.data() + start), packed) == Z_OK;
}

template <typename Visit>
void Children(const Binary& file, const Record& parent, Visit visit) {
    size_t offset = parent.children;
    while (offset < parent.end) {
        const Record child = ReadRecord(file, offset);
        if (child.null || child.end <= offset || child.end > file.data.size()) break;
        visit(child);
        offset = child.end;
    }
}

bool ScanBinary(const std::vector<char>& data, double* seconds) {
    Binary file{data};
    const uint32_t version = file.At<uint32_t>(23);
    file.wide = version >= 7500;
    double localStart = 0.0, localStop = 0.0;
    bool haveStack = false;
    double earliestKey = std::numeric_limits<double>::infinity();
    size_t offset = 27;
    while (offset < data.size()) {
        const Record top = ReadRecord(file, offset);
        if (top.null || top.end <= offset || top.end > data.size()) break;
        if (top.name == "Objects") {
            Children(file, top, [&](const Record& object) {
                if (object.name == "AnimationStack" && !haveStack) {
                    haveStack = true;
                    Children(file, object, [&](const Record& properties) {
                        if (properties.name != "Properties70") return;
                        Children(file, properties, [&](const Record& p) {
                            if (p.name != "P" || p.count < 5) return;
                            const std::string name = StringProperty(file, p.properties);
                            if (name != "LocalStart" && name != "LocalStop") return;
                            size_t at = p.properties;
                            for (int skip = 0; skip < 4; ++skip) at += PropertySize(file, at);
                            double value = 0.0;
                            if (NumberProperty(file, at, &value)) (name == "LocalStart" ? localStart : localStop) = value;
                        });
                    });
                } else if (object.name == "AnimationCurve") {
                    Children(file, object, [&](const Record& keys) {
                        if (keys.name != "KeyTime") return;
                        std::vector<int64_t> times;
                        if (Int64Array(file, keys.properties, &times) && !times.empty()) {
                            earliestKey = std::min(earliestKey, static_cast<double>(*std::min_element(times.begin(), times.end())));
                        }
                    });
                }
            });
        }
        offset = top.end;
    }
    if (haveStack && (localStart != 0.0 || localStop != 0.0)) {
        *seconds = localStart / kTicksPerSecond;
        return true;
    }
    if (earliestKey < std::numeric_limits<double>::infinity()) {
        *seconds = earliestKey / kTicksPerSecond;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- ASCII

// The numbers after `from`, up to the closing brace of the array.
void Numbers(const std::string& text, size_t from, std::vector<double>* out) {
    const size_t open = text.find('{', from);
    const size_t close = text.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return;
    size_t at = text.find("a:", open);
    if (at == std::string::npos || at > close) return;
    at += 2;
    while (at < close) {
        while (at < close && !(std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '-')) ++at;
        if (at >= close) break;
        size_t end = at;
        while (end < close && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-' ||
                               text[end] == '.' || text[end] == 'e' || text[end] == 'E' || text[end] == '+')) {
            ++end;
        }
        out->push_back(std::strtod(text.substr(at, end - at).c_str(), nullptr));
        at = end;
    }
}

bool LastNumberOnLine(const std::string& text, size_t from, double* value) {
    const size_t end = text.find('\n', from);
    const std::string line = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
    const size_t comma = line.rfind(',');
    if (comma == std::string::npos) return false;
    *value = std::strtod(line.c_str() + comma + 1, nullptr);
    return true;
}

bool ScanAscii(const std::string& text, double* seconds) {
    const size_t stack = text.find("AnimationStack:");
    if (stack != std::string::npos) {
        const size_t stackEnd = text.find("AnimationLayer:", stack);
        double localStart = 0.0, localStop = 0.0;
        const size_t start = text.find("\"LocalStart\"", stack);
        const size_t stop = text.find("\"LocalStop\"", stack);
        if (start != std::string::npos && (stackEnd == std::string::npos || start < stackEnd)) {
            LastNumberOnLine(text, start, &localStart);
        }
        if (stop != std::string::npos && (stackEnd == std::string::npos || stop < stackEnd)) {
            LastNumberOnLine(text, stop, &localStop);
        }
        if (localStart != 0.0 || localStop != 0.0) {
            *seconds = localStart / kTicksPerSecond;
            return true;
        }
    }
    double earliest = std::numeric_limits<double>::infinity();
    for (size_t at = text.find("KeyTime:"); at != std::string::npos; at = text.find("KeyTime:", at + 8)) {
        std::vector<double> times;
        Numbers(text, at, &times);
        for (const double t : times) earliest = std::min(earliest, t);
    }
    if (earliest < std::numeric_limits<double>::infinity()) {
        *seconds = earliest / kTicksPerSecond;
        return true;
    }
    return false;
}

}  // namespace

bool FbxAnimationStart(const std::filesystem::path& path, double* seconds) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    static const char kMagic[] = "Kaydara FBX Binary";
    if (data.size() > 27 && std::memcmp(data.data(), kMagic, sizeof(kMagic) - 1) == 0) return ScanBinary(data, seconds);
    return ScanAscii(std::string(data.begin(), data.end()), seconds);
}

}  // namespace importer
}  // namespace usdprep

#else

namespace usdprep {
namespace importer {
bool FbxAnimationStart(const std::filesystem::path&, double*) { return false; }
}  // namespace importer
}  // namespace usdprep

#endif
