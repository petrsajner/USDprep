// OBJ in: a Wavefront .obj (with its .mtl) turned into USD.
//
// What a 3D application shows when it opens the file is what comes out:
// every object (o) or group (g) is a mesh, its polygons as they are (no
// triangulation), with the UVs, normals and vertex colours the file has,
// and the .mtl materials as UsdPreviewSurface with their textures.
// Materials that change from face to face become GeomSubsets, the way a
// production scene has them (the Nuke export splits them later).
//
// Written with scans in mind: a lidar or photogrammetry OBJ is gigabytes
// of text, so the reader streams it and parses numbers without locale
// lookups, and positions are kept as floats relative to the first vertex.
// Scans often sit at survey coordinates millions of units from the
// origin, where a float cannot hold a centimetre: when a file does, its
// points are stored around their centre and the root carries the offset
// in double precision - the objects stay exactly where they were.

#include <usdprep/Import.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdprep {

namespace fs = std::filesystem;

namespace {

// ============================================================== small tools

fs::path PathOf(const std::string& utf8) { return fs::u8path(utf8); }
std::string Utf8Of(const fs::path& path) { return path.u8string(); }

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool EndsWithNoCase(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && Lower(text.substr(text.size() - n)) == suffix;
}

FILE* OpenForReading(const std::string& utf8) {
#ifdef _WIN32
    return _wfopen(PathOf(utf8).c_str(), L"rb");
#else
    return std::fopen(utf8.c_str(), "rb");
#endif
}

bool ReadWholeFile(const std::string& utf8, std::string* out) {
    FILE* file = OpenForReading(utf8);
    if (!file) return false;
    char buffer[1 << 16];
    size_t n = 0;
    out->clear();
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out->append(buffer, n);
    std::fclose(file);
    return true;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// 1234567 -> "1,234,567": the report is read by people.
std::string Count(size_t n) {
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

std::string Number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

// Prim names: letters, digits and underscores. Accented Latin letters
// lose their accents ("Předávka" -> "Predavka") rather than turning into
// underscores; anything else becomes an underscore.
const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOO_OUUUUYTsaaaaaaaceeeeiiiidnooooo_ouuuuyty";  // U+00C0..U+00FF
const char kLatinExtA[] =                                                                   // U+0100..U+017F
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static_assert(sizeof(kLatin1) == 0x40 + 1, "one letter per code point U+00C0..U+00FF");
static_assert(sizeof(kLatinExtA) == 0x80 + 1, "one letter per code point U+0100..U+017F");

std::string PrimNameFor(const std::string& utf8, const std::string& fallback) {
    std::string name;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            name += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
            ++i;
            continue;
        }
        // one UTF-8 sequence
        size_t length = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        unsigned int code = 0;
        if (length == 2 && i + 1 < utf8.size()) code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
        if (code >= 0xC0 && code < 0x100) {
            name += kLatin1[code - 0xC0];
        } else if (code >= 0x100 && code < 0x180) {
            name += kLatinExtA[code - 0x100];
        } else {
            name += '_';
        }
        i += length;
    }
    // no leading, trailing or doubled underscores left over from spaces and dots
    std::string tidy;
    for (const char c : name) {
        if (c == '_' && (tidy.empty() || tidy.back() == '_')) continue;
        tidy += c;
    }
    while (!tidy.empty() && tidy.back() == '_') tidy.pop_back();
    if (tidy.empty()) tidy = fallback;
    if (std::isdigit(static_cast<unsigned char>(tidy[0]))) tidy = "_" + tidy;
    return tidy;
}

std::string UniqueName(const std::string& wanted, std::unordered_map<std::string, int>* taken) {
    std::string name = wanted;
    for (int n = 2; (*taken)[name]++ > 0; ++n) name = wanted + "_" + std::to_string(n);
    return name;
}

// ================================================================ reading

// A file streamed line by line in large blocks. A line is [begin, end)
// without its line break; it is valid until the next call.
class LineReader {
  public:
    explicit LineReader(FILE* file) : _file(file), _buffer(kBlock) {}

    bool Next(const char** begin, const char** end) {
        for (;;) {
            const char* start = _buffer.data() + _pos;
            const char* stop = _buffer.data() + _size;
            const char* newline = static_cast<const char*>(std::memchr(start, '\n', static_cast<size_t>(stop - start)));
            if (newline || (_eof && start < stop)) {
                const char* lineEnd = newline ? newline : stop;
                *begin = start;
                *end = lineEnd;
                if (*end > *begin && (*end)[-1] == '\r') --*end;
                _pos = static_cast<size_t>(lineEnd - _buffer.data()) + (newline ? 1 : 0);
                _consumed += static_cast<uint64_t>(lineEnd - start) + (newline ? 1 : 0);
                return true;
            }
            if (_eof) return false;
            // keep the unfinished line, read the next block behind it
            const size_t partial = _size - _pos;
            if (partial > 0 && _pos > 0) std::memmove(_buffer.data(), _buffer.data() + _pos, partial);
            _size = partial;
            _pos = 0;
            if (_size == _buffer.size()) _buffer.resize(_buffer.size() * 2);  // one very long line
            const size_t got = std::fread(_buffer.data() + _size, 1, _buffer.size() - _size, _file);
            if (got == 0) _eof = true;
            _size += got;
        }
    }

    uint64_t Consumed() const { return _consumed; }

  private:
    static constexpr size_t kBlock = size_t(16) << 20;
    FILE* _file;
    std::vector<char> _buffer;
    size_t _pos = 0;
    size_t _size = 0;
    bool _eof = false;
    uint64_t _consumed = 0;
};

inline const char* SkipBlank(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

inline bool ParseDouble(const char*& p, const char* end, double* out) {
    p = SkipBlank(p, end);
    if (p < end && *p == '+') ++p;
    const auto result = std::from_chars(p, end, *out);
    if (result.ec != std::errc()) return false;
    p = result.ptr;
    return true;
}

inline bool ParseInt(const char*& p, const char* end, int* out) {
    if (p < end && *p == '+') ++p;
    const auto result = std::from_chars(p, end, *out);
    if (result.ec != std::errc()) return false;
    p = result.ptr;
    return true;
}

// The rest of a line, without the blanks around it.
std::string Rest(const char* p, const char* end) {
    p = SkipBlank(p, end);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
    return std::string(p, end);
}

// One object or group of the file: one mesh.
struct Part {
    std::string name;
    std::vector<int> counts;    // corners per polygon
    std::vector<int> v;         // per corner: position index
    std::vector<int> vt;        // per corner: UV index, -1 = none; empty = the part has no UVs
    std::vector<int> vn;        // per corner: normal index, same rules
    std::vector<int> material;  // per polygon: material id, -1 = none; empty = no usemtl in force
};

struct ObjData {
    GfVec3d origin{0.0};             // positions are stored relative to it (the first vertex)
    std::vector<GfVec3f> positions;
    std::vector<GfVec3f> colors;     // empty, or one per position
    bool colorsAbove1 = false;       // written 0-255
    std::vector<GfVec2f> uvs;
    std::vector<GfVec3f> normals;
    std::vector<Part> parts;
    std::vector<std::string> materialNames;  // index = material id
    std::vector<std::string> materialLibraries;
    // what was left out
    size_t shortFaces = 0;
    size_t brokenFaces = 0;
    size_t lines = 0;
    size_t points = 0;
    size_t freeform = 0;
};

// Resolves an OBJ index (1-based, or negative = counted back from the
// last one) against `count` elements. -1 = out of range.
inline int Resolve(int index, size_t count) {
    const long long resolved = index > 0 ? static_cast<long long>(index) - 1 : static_cast<long long>(count) + index;
    return (index != 0 && resolved >= 0 && resolved < static_cast<long long>(count)) ? static_cast<int>(resolved) : -1;
}

bool ParseObj(const std::string& path, const std::string& defaultName, ObjData* data, ImportProgress* progress,
              std::string* error) {
    FILE* file = OpenForReading(path);
    if (!file) {
        *error = "cannot open the file for reading";
        return false;
    }
    std::error_code sizeError;
    const uint64_t fileSize = fs::file_size(PathOf(path), sizeError);

    LineReader reader(file);
    std::unordered_map<std::string, size_t> partIndex;
    std::unordered_map<std::string, int> materialIndex;
    size_t current = SIZE_MAX;
    int currentMaterial = -1;
    bool haveOrigin = false;

    const auto partNamed = [&](const std::string& name) {
        const auto found = partIndex.find(name);
        if (found != partIndex.end()) return found->second;
        data->parts.emplace_back();
        data->parts.back().name = name;
        partIndex[name] = data->parts.size() - 1;
        return data->parts.size() - 1;
    };

    const char* begin = nullptr;
    const char* end = nullptr;
    size_t lineNumber = 0;
    bool firstLine = true;
    while (reader.Next(&begin, &end)) {
        if (firstLine) {
            firstLine = false;
            if (end - begin >= 3 && std::memcmp(begin, "\xEF\xBB\xBF", 3) == 0) begin += 3;  // UTF-8 byte order mark
        }
        if ((++lineNumber & 0xFFFF) == 0 && progress) {
            if (progress->cancel) {
                std::fclose(file);
                *error = "cancelled";
                return false;
            }
            if (fileSize > 0) progress->fraction = 0.8f * static_cast<float>(reader.Consumed()) / static_cast<float>(fileSize);
        }
        const char* p = SkipBlank(begin, end);
        if (p >= end || *p == '#') continue;
        const char* keyEnd = p;
        while (keyEnd < end && *keyEnd != ' ' && *keyEnd != '\t') ++keyEnd;
        const size_t keyLength = static_cast<size_t>(keyEnd - p);
        const auto is = [&](const char* keyword) {
            return keyLength == std::strlen(keyword) && std::memcmp(p, keyword, keyLength) == 0;
        };

        if (is("v")) {
            double values[7];
            int n = 0;
            const char* q = keyEnd;
            while (n < 7 && ParseDouble(q, end, &values[n])) ++n;
            if (n < 3) continue;
            if (!haveOrigin) {
                data->origin = GfVec3d(values[0], values[1], values[2]);
                haveOrigin = true;
            }
            data->positions.emplace_back(static_cast<float>(values[0] - data->origin[0]),
                                         static_cast<float>(values[1] - data->origin[1]),
                                         static_cast<float>(values[2] - data->origin[2]));
            if (n >= 6) {
                // v x y z r g b: the colour of a scan point
                if (data->colors.size() + 1 < data->positions.size()) {
                    data->colors.resize(data->positions.size() - 1, GfVec3f(1.0f));
                }
                const GfVec3f color(static_cast<float>(values[3]), static_cast<float>(values[4]),
                                    static_cast<float>(values[5]));
                if (color[0] > 1.0f || color[1] > 1.0f || color[2] > 1.0f) data->colorsAbove1 = true;
                data->colors.push_back(color);
            } else if (!data->colors.empty()) {
                data->colors.emplace_back(1.0f);
            }
        } else if (is("vt")) {
            double u = 0.0, v = 0.0;
            const char* q = keyEnd;
            if (!ParseDouble(q, end, &u)) continue;
            ParseDouble(q, end, &v);
            data->uvs.emplace_back(static_cast<float>(u), static_cast<float>(v));
        } else if (is("vn")) {
            double x = 0.0, y = 0.0, z = 0.0;
            const char* q = keyEnd;
            if (!ParseDouble(q, end, &x) || !ParseDouble(q, end, &y) || !ParseDouble(q, end, &z)) continue;
            data->normals.emplace_back(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        } else if (is("f")) {
            if (current == SIZE_MAX) current = partNamed(defaultName);
            Part& part = data->parts[current];
            const size_t start = part.v.size();
            int corners = 0;
            bool broken = false;
            const char* q = keyEnd;
            for (;;) {
                q = SkipBlank(q, end);
                if (q >= end) break;
                int vi = 0, ti = 0, ni = 0;
                bool hasT = false, hasN = false;
                if (!ParseInt(q, end, &vi)) {
                    broken = true;
                    break;
                }
                if (q < end && *q == '/') {
                    ++q;
                    if (q < end && *q != '/') hasT = ParseInt(q, end, &ti);
                    if (q < end && *q == '/') {
                        ++q;
                        hasN = ParseInt(q, end, &ni);
                    }
                }
                while (q < end && *q != ' ' && *q != '\t') ++q;  // whatever else the token holds
                const int v = Resolve(vi, data->positions.size());
                const int t = hasT ? Resolve(ti, data->uvs.size()) : -1;
                const int n = hasN ? Resolve(ni, data->normals.size()) : -1;
                if (v < 0 || (hasT && t < 0) || (hasN && n < 0)) {
                    broken = true;
                    break;
                }
                const size_t corner = part.v.size();
                part.v.push_back(v);
                if (hasT) {
                    if (part.vt.size() < corner) part.vt.resize(corner, -1);
                    part.vt.push_back(t);
                } else if (!part.vt.empty()) {
                    part.vt.push_back(-1);
                }
                if (hasN) {
                    if (part.vn.size() < corner) part.vn.resize(corner, -1);
                    part.vn.push_back(n);
                } else if (!part.vn.empty()) {
                    part.vn.push_back(-1);
                }
                ++corners;
            }
            if (broken || corners < 3) {
                ++(broken ? data->brokenFaces : data->shortFaces);
                part.v.resize(start);
                if (part.vt.size() > start) part.vt.resize(start);
                if (part.vn.size() > start) part.vn.resize(start);
                continue;
            }
            part.counts.push_back(corners);
            if (currentMaterial >= 0 || !part.material.empty()) {
                if (part.material.size() + 1 < part.counts.size()) part.material.resize(part.counts.size() - 1, -1);
                part.material.push_back(currentMaterial);
            }
        } else if (is("o") || is("g")) {
            const std::string name = Rest(keyEnd, end);
            current = partNamed(name.empty() ? defaultName : name);
        } else if (is("usemtl")) {
            const std::string name = Rest(keyEnd, end);
            if (name.empty()) {
                currentMaterial = -1;
            } else {
                const auto found = materialIndex.find(name);
                if (found != materialIndex.end()) {
                    currentMaterial = found->second;
                } else {
                    currentMaterial = static_cast<int>(data->materialNames.size());
                    materialIndex[name] = currentMaterial;
                    data->materialNames.push_back(name);
                }
            }
        } else if (is("mtllib")) {
            const std::string name = Rest(keyEnd, end);
            if (!name.empty()) data->materialLibraries.push_back(name);
        } else if (is("l")) {
            ++data->lines;
        } else if (is("p")) {
            ++data->points;
        } else if (is("vp") || is("cstype") || is("curv") || is("curv2") || is("surf")) {
            ++data->freeform;
        }
        // s, lod, maplib, usemap, ... change nothing a mesh can show
    }
    std::fclose(file);

    if (!data->colors.empty() && data->colors.size() < data->positions.size()) {
        data->colors.resize(data->positions.size(), GfVec3f(1.0f));
    }
    // parts that only ever held vertices ("g default" before the vertex list)
    data->parts.erase(std::remove_if(data->parts.begin(), data->parts.end(),
                                     [](const Part& part) { return part.counts.empty(); }),
                      data->parts.end());
    for (Part& part : data->parts) {
        if (!part.material.empty() && part.material.size() < part.counts.size()) {
            part.material.resize(part.counts.size(), -1);
        }
    }
    return true;
}

// ================================================================ materials

struct MtlMap {
    std::string file;              // as written in the .mtl
    std::string channel;           // -imfchan: r g b m l z, empty = the map's natural channel
    GfVec2f scale{1.0f, 1.0f};     // -s
    GfVec2f offset{0.0f, 0.0f};    // -o
    bool bumpMultiplier = false;   // -bm: how Blender marks a normal map
    bool Set() const { return !file.empty(); }
};

struct Mtl {
    std::string name;
    bool hasKd = false;
    GfVec3f kd{0.8f};
    MtlMap mapKd;
    bool hasD = false;
    float d = 1.0f;
    MtlMap mapD;
    bool hasNs = false;
    float ns = 0.0f;
    bool hasPr = false;
    float pr = 0.5f;
    MtlMap mapPr;
    bool hasPm = false;
    float pm = 0.0f;
    MtlMap mapPm;
    bool hasKe = false;
    GfVec3f ke{0.0f};
    MtlMap mapKe;
    MtlMap mapNormal;
    MtlMap mapBump;  // a height map, unless it turns out to be a normal map
};

// The options in front of a map's file name: -o 0.5 0.5, -s 2 2, -bm 1 ...
MtlMap ParseMap(const std::string& line) {
    MtlMap map;
    const char* p = line.c_str();
    const char* end = p + line.size();
    for (;;) {
        p = SkipBlank(p, end);
        if (p >= end || *p != '-') break;
        const char* optionEnd = p;
        while (optionEnd < end && *optionEnd != ' ' && *optionEnd != '\t') ++optionEnd;
        const std::string option = Lower(std::string(p, optionEnd));
        p = optionEnd;
        const auto word = [&]() {
            p = SkipBlank(p, end);
            const char* wordEnd = p;
            while (wordEnd < end && *wordEnd != ' ' && *wordEnd != '\t') ++wordEnd;
            std::string text(p, wordEnd);
            p = wordEnd;
            return text;
        };
        const auto numbers = [&](GfVec2f* into) {
            double value = 0.0;
            for (int i = 0; i < 3; ++i) {
                const char* before = p;
                if (!ParseDouble(p, end, &value)) {
                    p = before;
                    break;
                }
                if (into && i < 2) (*into)[i] = static_cast<float>(value);
            }
        };
        if (option == "-s") {
            numbers(&map.scale);
        } else if (option == "-o") {
            numbers(&map.offset);
        } else if (option == "-t") {
            numbers(nullptr);
        } else if (option == "-mm") {
            word();
            word();
        } else if (option == "-imfchan") {
            map.channel = Lower(word());
        } else if (option == "-bm") {
            map.bumpMultiplier = true;
            word();
        } else if (option == "-blendu" || option == "-blendv" || option == "-boost" || option == "-texres" ||
                   option == "-clamp" || option == "-type" || option == "-cc") {
            word();
        }
        // an unknown option is skipped on its own
    }
    map.file = Rest(p, end);
    return map;
}

bool LooksLikeNormalMap(const MtlMap& map) {
    const std::string name = Lower(PathOf(map.file).filename().u8string());
    return map.bumpMultiplier || name.find("normal") != std::string::npos || name.find("nrm") != std::string::npos ||
           name.find("_nor") != std::string::npos || name.find("_n.") != std::string::npos;
}

void ParseMtl(const std::string& text, std::unordered_map<std::string, Mtl>* out) {
    Mtl* current = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::string line = text.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const char* p = SkipBlank(line.c_str(), line.c_str() + line.size());
        const char* end = line.c_str() + line.size();
        if (p >= end || *p == '#') continue;
        const char* keyEnd = p;
        while (keyEnd < end && *keyEnd != ' ' && *keyEnd != '\t') ++keyEnd;
        const std::string key = Lower(std::string(p, keyEnd));
        const std::string rest = Rest(keyEnd, end);
        if (key == "newmtl") {
            current = &(*out)[rest];
            current->name = rest;
            continue;
        }
        if (!current) continue;
        const auto color = [&](GfVec3f* into) {
            const char* q = rest.c_str();
            const char* qEnd = q + rest.size();
            double r = 0.0, g = 0.0, b = 0.0;
            if (!ParseDouble(q, qEnd, &r)) return false;
            if (!ParseDouble(q, qEnd, &g) || !ParseDouble(q, qEnd, &b)) g = b = r;
            *into = GfVec3f(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
            return true;
        };
        const auto scalar = [&](float* into) {
            const char* q = rest.c_str();
            double value = 0.0;
            if (!ParseDouble(q, q + rest.size(), &value)) return false;
            *into = static_cast<float>(value);
            return true;
        };
        if (key == "kd") {
            current->hasKd = color(&current->kd);
        } else if (key == "map_kd") {
            current->mapKd = ParseMap(rest);
        } else if (key == "d") {
            current->hasD = scalar(&current->d);
        } else if (key == "tr") {
            float tr = 0.0f;
            if (scalar(&tr)) {
                current->hasD = true;
                current->d = 1.0f - tr;
            }
        } else if (key == "map_d") {
            current->mapD = ParseMap(rest);
        } else if (key == "ns") {
            current->hasNs = scalar(&current->ns);
        } else if (key == "pr") {
            current->hasPr = scalar(&current->pr);
        } else if (key == "map_pr") {
            current->mapPr = ParseMap(rest);
        } else if (key == "pm") {
            current->hasPm = scalar(&current->pm);
        } else if (key == "map_pm") {
            current->mapPm = ParseMap(rest);
        } else if (key == "ke") {
            current->hasKe = color(&current->ke);
        } else if (key == "map_ke") {
            current->mapKe = ParseMap(rest);
        } else if (key == "norm" || key == "map_norm") {
            current->mapNormal = ParseMap(rest);
        } else if (key == "map_bump" || key == "bump") {
            current->mapBump = ParseMap(rest);
        }
    }
}

// Where a texture named in an .mtl really is. Relative names are next to
// the .mtl; a path from another machine is looked for there by its bare
// file name. Missing textures keep their path: the export names them.
std::string ResolveTexture(const std::string& written, const fs::path& mtlDir, std::vector<std::string>* foundElsewhere,
                           std::vector<std::string>* missing) {
    std::string cleaned = written;
    std::replace(cleaned.begin(), cleaned.end(), '\\', '/');
    fs::path path = PathOf(cleaned);
    if (path.is_relative()) path = mtlDir / path;
    if (IsFile(path)) return path.lexically_normal().generic_u8string();
    const fs::path nextTo = mtlDir / path.filename();
    if (IsFile(nextTo)) {
        foundElsewhere->push_back(Utf8Of(path.filename()));
        return nextTo.lexically_normal().generic_u8string();
    }
    missing->push_back(written);
    return path.lexically_normal().generic_u8string();
}

class MaterialWriter {
  public:
    MaterialWriter(const UsdStageRefPtr& stage, const fs::path& mtlDir) : _stage(stage), _mtlDir(mtlDir) {}

    UsdShadeMaterial Write(const SdfPath& path, const Mtl& mtl, Report& rep) {
        _path = path;
        _reader = UsdShadeOutput();
        UsdShadeMaterial material = UsdShadeMaterial::Define(_stage, path);
        _surface = UsdShadeShader::Define(_stage, path.AppendChild(TfToken("PreviewSurface")));
        _surface.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        material.CreateSurfaceOutput().ConnectToSource(_surface.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token));

        // colour: the texture wins over Kd, as in the applications that write both
        UsdShadeShader diffuse;
        if (mtl.mapKd.Set()) {
            diffuse = Texture("diffuseTexture", mtl.mapKd, false);
            Connect("diffuseColor", SdfValueTypeNames->Color3f, diffuse, "rgb", SdfValueTypeNames->Float3);
        } else if (mtl.hasKd) {
            _surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(mtl.kd);
        }

        if (mtl.mapD.Set()) {
            // the diffuse texture's own alpha, or a separate mask
            const bool sameAsColor = diffuse && mtl.mapD.file == mtl.mapKd.file;
            const std::string channel = !mtl.mapD.channel.empty() ? Channel(mtl.mapD.channel) : sameAsColor ? "a" : "r";
            Connect("opacity", SdfValueTypeNames->Float, sameAsColor ? diffuse : Texture("opacityTexture", mtl.mapD, true),
                    channel, SdfValueTypeNames->Float);
        } else if (mtl.hasD && mtl.d < 1.0f) {
            if (mtl.d <= 0.0f) {
                rep.Warn("import", "material " + mtl.name +
                                       " says it is fully transparent (d 0 / Tr 1) - read as opaque: that is an "
                                       "exporter's mistake far more often than an invisible object");
            } else {
                _surface.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(mtl.d);
            }
        }

        if (mtl.mapPr.Set()) {
            Connect("roughness", SdfValueTypeNames->Float, Texture("roughnessTexture", mtl.mapPr, true),
                    Channel(mtl.mapPr.channel), SdfValueTypeNames->Float);
        } else if (mtl.hasPr) {
            _surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(std::clamp(mtl.pr, 0.0f, 1.0f));
        } else if (mtl.hasNs) {
            // the specular exponent (0-1000) as roughness, the way Blender converts it
            const float roughness = 1.0f - std::sqrt(std::clamp(mtl.ns, 0.0f, 1000.0f) / 1000.0f);
            _surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(roughness);
        }

        if (mtl.mapPm.Set()) {
            Connect("metallic", SdfValueTypeNames->Float, Texture("metallicTexture", mtl.mapPm, true),
                    Channel(mtl.mapPm.channel), SdfValueTypeNames->Float);
        } else if (mtl.hasPm) {
            _surface.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(std::clamp(mtl.pm, 0.0f, 1.0f));
        }

        if (mtl.mapKe.Set()) {
            Connect("emissiveColor", SdfValueTypeNames->Color3f, Texture("emissiveTexture", mtl.mapKe, false), "rgb",
                    SdfValueTypeNames->Float3);
        } else if (mtl.hasKe && (mtl.ke[0] > 0.0f || mtl.ke[1] > 0.0f || mtl.ke[2] > 0.0f)) {
            _surface.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set(mtl.ke);
        }

        const MtlMap* normal = mtl.mapNormal.Set() ? &mtl.mapNormal
                               : (mtl.mapBump.Set() && LooksLikeNormalMap(mtl.mapBump)) ? &mtl.mapBump
                                                                                         : nullptr;
        if (normal) {
            UsdShadeShader texture = Texture("normalTexture", *normal, true);
            texture.CreateInput(TfToken("scale"), SdfValueTypeNames->Float4).Set(GfVec4f(2.0f, 2.0f, 2.0f, 1.0f));
            texture.CreateInput(TfToken("bias"), SdfValueTypeNames->Float4).Set(GfVec4f(-1.0f, -1.0f, -1.0f, 0.0f));
            Connect("normal", SdfValueTypeNames->Normal3f, texture, "rgb", SdfValueTypeNames->Float3);
        } else if (mtl.mapBump.Set()) {
            rep.Warn("import", "material " + mtl.name + ": bump map " + Utf8Of(PathOf(mtl.mapBump.file).filename()) +
                                   " left out - a height map, and the preview material has no input for one");
        }
        return material;
    }

    std::vector<std::string> foundElsewhere;
    std::vector<std::string> missing;
    size_t textures = 0;

  private:
    static std::string Channel(const std::string& imfchan) {
        if (imfchan == "g") return "g";
        if (imfchan == "b") return "b";
        if (imfchan == "m") return "a";
        return "r";  // r, l (luminance), z, or nothing said
    }

    UsdShadeOutput StReader() {
        if (!_reader) {
            UsdShadeShader reader = UsdShadeShader::Define(_stage, _path.AppendChild(TfToken("stReader")));
            reader.CreateIdAttr(VtValue(TfToken("UsdPrimvarReader_float2")));
            reader.CreateInput(TfToken("varname"), SdfValueTypeNames->Token).Set(TfToken("st"));
            _reader = reader.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2);
        }
        return _reader;
    }

    UsdShadeShader Texture(const std::string& nodeName, const MtlMap& map, bool data) {
        UsdShadeShader texture = UsdShadeShader::Define(_stage, _path.AppendChild(TfToken(nodeName)));
        texture.CreateIdAttr(VtValue(TfToken("UsdUVTexture")));
        texture.CreateInput(TfToken("file"), SdfValueTypeNames->Asset)
            .Set(SdfAssetPath(ResolveTexture(map.file, _mtlDir, &foundElsewhere, &missing)));
        UsdShadeOutput st = StReader();
        if (map.scale != GfVec2f(1.0f, 1.0f) || map.offset != GfVec2f(0.0f, 0.0f)) {
            UsdShadeShader transform =
                UsdShadeShader::Define(_stage, _path.AppendChild(TfToken(nodeName + "_transform")));
            transform.CreateIdAttr(VtValue(TfToken("UsdTransform2d")));
            transform.CreateInput(TfToken("in"), SdfValueTypeNames->Float2).ConnectToSource(st);
            transform.CreateInput(TfToken("scale"), SdfValueTypeNames->Float2).Set(map.scale);
            transform.CreateInput(TfToken("translation"), SdfValueTypeNames->Float2).Set(map.offset);
            st = transform.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2);
        }
        texture.CreateInput(TfToken("st"), SdfValueTypeNames->Float2).ConnectToSource(st);
        texture.CreateInput(TfToken("wrapS"), SdfValueTypeNames->Token).Set(TfToken("repeat"));
        texture.CreateInput(TfToken("wrapT"), SdfValueTypeNames->Token).Set(TfToken("repeat"));
        if (data) texture.CreateInput(TfToken("sourceColorSpace"), SdfValueTypeNames->Token).Set(TfToken("raw"));
        ++textures;
        return texture;
    }

    void Connect(const char* input, const SdfValueTypeName& inputType, UsdShadeShader source,
                 const std::string& output, const SdfValueTypeName& outputType) {
        _surface.CreateInput(TfToken(input), inputType)
            .ConnectToSource(source.CreateOutput(TfToken(output), outputType));
    }

    UsdStageRefPtr _stage;
    fs::path _mtlDir;
    SdfPath _path;
    UsdShadeShader _surface;
    UsdShadeOutput _reader;
};

// ================================================================ meshes

struct MeshCounts {
    size_t points = 0;
    size_t faces = 0;
    bool partialUvs = false;
    bool partialNormals = false;
};

// Per-corner indices (-1 = none) that always agree for a given point can
// be stored once per point; otherwise they stay per corner.
bool OnePerPoint(const std::vector<int>& perCorner, const VtIntArray& cornerPoint, size_t pointCount,
                 std::vector<int>* perPoint) {
    perPoint->assign(pointCount, -2);
    for (size_t c = 0; c < perCorner.size(); ++c) {
        int& slot = (*perPoint)[static_cast<size_t>(cornerPoint[c])];
        if (perCorner[c] < 0) return false;
        if (slot == -2) {
            slot = perCorner[c];
        } else if (slot != perCorner[c]) {
            return false;
        }
    }
    return true;
}

MeshCounts WriteMesh(const ObjData& data, const Part& part, UsdGeomMesh mesh, const GfVec3d& shift,
                     std::vector<int>* localOf) {
    MeshCounts counts;

    // the points this part uses, in the order it uses them
    VtIntArray indices(part.v.size());
    std::vector<int> used;
    for (size_t c = 0; c < part.v.size(); ++c) {
        int& local = (*localOf)[static_cast<size_t>(part.v[c])];
        if (local < 0) {
            local = static_cast<int>(used.size());
            used.push_back(part.v[c]);
        }
        indices[c] = local;
    }
    VtVec3fArray points(used.size());
    for (size_t i = 0; i < used.size(); ++i) {
        const GfVec3f& p = data.positions[static_cast<size_t>(used[i])];
        points[i] = GfVec3f(static_cast<float>(p[0] + shift[0]), static_cast<float>(p[1] + shift[1]),
                            static_cast<float>(p[2] + shift[2]));
    }
    for (const int global : used) (*localOf)[static_cast<size_t>(global)] = -1;  // ready for the next part

    VtIntArray faceCounts(part.counts.begin(), part.counts.end());
    mesh.CreateFaceVertexCountsAttr().Set(faceCounts);
    mesh.CreateFaceVertexIndicesAttr().Set(indices);
    mesh.CreatePointsAttr().Set(points);
    // polygons as they are: an OBJ is not a subdivision cage
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    VtVec3fArray extent;
    if (UsdGeomPointBased::ComputeExtent(points, &extent)) mesh.CreateExtentAttr().Set(extent);

    UsdGeomPrimvarsAPI primvars(mesh);
    if (!data.colors.empty()) {
        const float scale = data.colorsAbove1 ? 1.0f / 255.0f : 1.0f;
        VtVec3fArray colors(used.size());
        for (size_t i = 0; i < used.size(); ++i) colors[i] = data.colors[static_cast<size_t>(used[i])] * scale;
        mesh.CreateDisplayColorPrimvar(UsdGeomTokens->vertex).Set(colors);
    }

    std::vector<int> perPoint;
    if (!part.vt.empty()) {
        counts.partialUvs = std::find(part.vt.begin(), part.vt.end(), -1) != part.vt.end();
        UsdGeomPrimvar st = primvars.CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray);
        if (OnePerPoint(part.vt, indices, used.size(), &perPoint)) {
            VtVec2fArray values(used.size());
            for (size_t i = 0; i < used.size(); ++i) values[i] = data.uvs[static_cast<size_t>(perPoint[i])];
            st.SetInterpolation(UsdGeomTokens->vertex);
            st.Set(values);
        } else {
            // indexed per corner, like a production mesh; corners without a UV share (0, 0)
            std::unordered_map<int, int> localUv;
            VtVec2fArray values;
            VtIntArray uvIndices(part.vt.size());
            for (size_t c = 0; c < part.vt.size(); ++c) {
                const auto found = localUv.find(part.vt[c]);
                if (found != localUv.end()) {
                    uvIndices[c] = found->second;
                    continue;
                }
                const int local = static_cast<int>(values.size());
                localUv[part.vt[c]] = local;
                values.push_back(part.vt[c] >= 0 ? data.uvs[static_cast<size_t>(part.vt[c])] : GfVec2f(0.0f));
                uvIndices[c] = local;
            }
            st.SetInterpolation(UsdGeomTokens->faceVarying);
            st.Set(values);
            st.SetIndices(uvIndices);
        }
    }

    if (!part.vn.empty()) {
        if (std::find(part.vn.begin(), part.vn.end(), -1) != part.vn.end()) {
            counts.partialNormals = true;  // half a set of normals is worse than none
        } else if (OnePerPoint(part.vn, indices, used.size(), &perPoint)) {
            VtVec3fArray values(used.size());
            for (size_t i = 0; i < used.size(); ++i) values[i] = data.normals[static_cast<size_t>(perPoint[i])];
            mesh.CreateNormalsAttr().Set(values);
            mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
        } else {
            VtVec3fArray values(part.vn.size());
            for (size_t c = 0; c < part.vn.size(); ++c) values[c] = data.normals[static_cast<size_t>(part.vn[c])];
            mesh.CreateNormalsAttr().Set(values);
            mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
        }
    }

    counts.points = used.size();
    counts.faces = part.counts.size();
    return counts;
}

}  // namespace

// ============================================================== the import

bool IsImportable(const std::string& path) { return EndsWithNoCase(path, ".obj"); }

Report ImportToUsd(const std::string& inputPath, const std::string& usdPath, ImportProgress* progress) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    if (!IsImportable(inputPath)) {
        rep.Fail("USDprep imports .obj files; this is not one: " + inputPath);
        return rep;
    }
    const fs::path source = PathOf(inputPath);
    std::error_code ec;
    const fs::path sourceAbsolute = fs::absolute(source, ec);
    const std::string stem = Utf8Of(source.stem());
    const std::string rootName = PrimNameFor(stem, "model");

    // ----- read ------------------------------------------------------------------
    ObjData data;
    std::string error;
    if (!ParseObj(inputPath, stem, &data, progress, &error)) {
        rep.Fail(error == "cancelled" ? "cancelled" : "cannot read " + inputPath + ": " + error);
        return rep;
    }
    if (data.parts.empty()) {
        rep.Fail(data.positions.empty() ? "no geometry in " + Utf8Of(source.filename())
                                        : "no polygons in " + Utf8Of(source.filename()) +
                                              " (points and lines only: USDprep reads polygon meshes)");
        return rep;
    }

    // ----- materials ---------------------------------------------------------------
    std::unordered_map<std::string, Mtl> materials;
    std::vector<std::string> missingLibraries;
    const auto loadLibrary = [&](std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        fs::path path = PathOf(name);
        if (path.is_relative()) path = source.parent_path() / path;
        if (!IsFile(path)) path = source.parent_path() / path.filename();  // written on another machine
        std::string text;
        if (!IsFile(path) || !ReadWholeFile(Utf8Of(path), &text)) return false;
        ParseMtl(text, &materials);
        return true;
    };
    for (const std::string& library : data.materialLibraries) {
        // a name with spaces is one file, or several
        if (loadLibrary(library)) continue;
        bool any = false;
        size_t start = 0;
        while (start < library.size()) {
            size_t space = library.find(' ', start);
            if (space == std::string::npos) space = library.size();
            if (space > start) any |= loadLibrary(library.substr(start, space - start));
            start = space + 1;
        }
        if (!any) missingLibraries.push_back(library);
    }
    for (const std::string& library : missingLibraries) {
        rep.Warn("import", "material library " + library + " not found - the objects come without their materials");
    }

    // ----- where the points go ------------------------------------------------------
    GfRange3d bounds;
    for (const GfVec3f& p : data.positions) bounds.UnionWith(GfVec3d(p));
    GfVec3d shift = data.origin;  // stored relative + origin = where the file puts them
    GfVec3d pivot(0.0);
    if (!bounds.IsEmpty()) {
        const GfVec3d low = bounds.GetMin() + data.origin;
        const GfVec3d high = bounds.GetMax() + data.origin;
        const double size = (high - low).GetLength();
        double farthest = 0.0;
        for (int i = 0; i < 3; ++i) farthest = std::max({farthest, std::fabs(low[i]), std::fabs(high[i])});
        // a float carries 7 digits: a thousand sizes away, a pixel starts to shake
        if (size > 0.0 && farthest > 1000.0 * size) {
            pivot = (low + high) * 0.5;
            shift = data.origin - pivot;
            rep.Info("import", "the model sits far from the origin (around " + Number(pivot[0]) + ", " +
                                   Number(pivot[1]) + ", " + Number(pivot[2]) +
                                   "): its points are stored around that centre and the root carries the offset in "
                                   "double precision - float points would lose detail out there. It stays where it was.");
        }
    }

    // ----- write -------------------------------------------------------------------
    if (progress) progress->fraction = 0.8f;
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);  // OBJ has no up axis; Y is what every application assumes
    UsdGeomXform root = UsdGeomXform::Define(stage, SdfPath::AbsoluteRootPath().AppendChild(TfToken(rootName)));
    stage->SetDefaultPrim(root.GetPrim());
    if (pivot != GfVec3d(0.0)) root.AddTranslateOp(UsdGeomXformOp::PrecisionDouble).Set(pivot);
    VtDictionary layerData;
    layerData[kImportedFromKey] = VtValue(Utf8Of(sourceAbsolute.empty() ? source : sourceAbsolute));
    stage->GetRootLayer()->SetCustomLayerData(layerData);

    std::unordered_map<std::string, int> taken;
    taken["Materials"] = 1;

    // the materials the polygons use, defined or not
    std::vector<UsdShadeMaterial> materialPrims(data.materialNames.size());
    std::vector<std::string> undefined;
    MaterialWriter writer(stage, source.parent_path());
    std::unordered_map<std::string, int> materialNamesTaken;
    for (size_t id = 0; id < data.materialNames.size(); ++id) {
        const auto found = materials.find(data.materialNames[id]);
        if (found == materials.end()) {
            undefined.push_back(data.materialNames[id]);
            continue;
        }
        const std::string name = UniqueName(PrimNameFor(data.materialNames[id], "material"), &materialNamesTaken);
        const SdfPath path = root.GetPath().AppendChild(TfToken("Materials")).AppendChild(TfToken(name));
        if (!stage->GetPrimAtPath(path.GetParentPath())) UsdGeomScope::Define(stage, path.GetParentPath());
        materialPrims[id] = writer.Write(path, found->second, rep);
    }
    if (!undefined.empty() && missingLibraries.empty()) {
        std::string names;
        for (size_t i = 0; i < undefined.size() && i < 4; ++i) names += (i ? ", " : "") + undefined[i];
        if (undefined.size() > 4) names += ", ...";
        rep.Warn("import", std::to_string(undefined.size()) + " material(s) used but not defined in the .mtl: " + names);
    }

    std::vector<int> localOf(data.positions.size(), -1);
    size_t pointsTotal = 0, facesTotal = 0, withSubsets = 0, partialUvs = 0, partialNormals = 0;
    for (size_t i = 0; i < data.parts.size(); ++i) {
        if (progress) {
            if (progress->cancel) {
                rep.Fail("cancelled");
                return rep;
            }
            progress->fraction = 0.8f + 0.1f * static_cast<float>(i) / static_cast<float>(data.parts.size());
        }
        const Part& part = data.parts[i];
        const std::string name = UniqueName(PrimNameFor(part.name, rootName), &taken);
        UsdGeomMesh mesh = UsdGeomMesh::Define(stage, root.GetPath().AppendChild(TfToken(name)));
        const MeshCounts counts = WriteMesh(data, part, mesh, shift, &localOf);
        pointsTotal += counts.points;
        facesTotal += counts.faces;
        partialUvs += counts.partialUvs ? 1 : 0;
        partialNormals += counts.partialNormals ? 1 : 0;

        // materials: one for the whole mesh, or one per group of polygons
        std::vector<std::vector<int>> facesOf(data.materialNames.size());
        for (size_t f = 0; f < part.material.size(); ++f) {
            const int id = part.material[f];
            if (id >= 0 && materialPrims[static_cast<size_t>(id)]) facesOf[static_cast<size_t>(id)].push_back(static_cast<int>(f));
        }
        std::vector<size_t> usedIds;
        for (size_t id = 0; id < facesOf.size(); ++id) {
            if (!facesOf[id].empty()) usedIds.push_back(id);
        }
        if (usedIds.size() == 1 && facesOf[usedIds[0]].size() == part.counts.size()) {
            UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(materialPrims[usedIds[0]]);
        } else if (!usedIds.empty()) {
            ++withSubsets;
            std::unordered_map<std::string, int> subsetNames;
            for (const size_t id : usedIds) {
                const std::string subsetName =
                    UniqueName(materialPrims[id].GetPrim().GetName().GetString(), &subsetNames);
                UsdGeomSubset subset = UsdGeomSubset::CreateGeomSubset(
                    mesh, TfToken(subsetName), UsdGeomTokens->face, VtIntArray(facesOf[id].begin(), facesOf[id].end()),
                    UsdShadeTokens->materialBind, UsdGeomTokens->nonOverlapping);
                UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(materialPrims[id]);
            }
        }
    }

    // ----- the report ---------------------------------------------------------------
    size_t materialCount = 0;
    for (const UsdShadeMaterial& material : materialPrims) materialCount += material ? 1 : 0;
    rep.Info("import", Utf8Of(source.filename()) + ": " + Count(data.parts.size()) + " object(s), " +
                           Count(facesTotal) + " polygons, " + Count(pointsTotal) + " points, " +
                           Count(materialCount) + " material(s), " + Count(writer.textures) + " texture(s)");
    if (!data.colors.empty()) {
        rep.Info("import", std::string("vertex colours kept as display colour") +
                               (data.colorsAbove1 ? " (written 0-255, scaled to 0-1)" : ""));
    }
    if (withSubsets > 0) {
        rep.Info("import", std::to_string(withSubsets) +
                               " object(s) change material from polygon to polygon - kept as material subsets");
    }
    if (!writer.foundElsewhere.empty()) {
        rep.Info("import", std::to_string(writer.foundElsewhere.size()) +
                               " texture(s) found next to the .mtl, not where it says (" + writer.foundElsewhere.front() +
                               (writer.foundElsewhere.size() > 1 ? ", ..." : "") + ")");
    }
    if (!writer.missing.empty()) {
        rep.Warn("import", std::to_string(writer.missing.size()) + " texture(s) not found: " + writer.missing.front() +
                               (writer.missing.size() > 1 ? ", ..." : ""));
    }
    if (partialUvs > 0) {
        rep.Warn("import", std::to_string(partialUvs) +
                               " object(s) have UVs on only some polygons - the others get UV (0, 0)");
    }
    if (partialNormals > 0) {
        rep.Warn("import", std::to_string(partialNormals) +
                               " object(s) have normals on only some polygons - their normals are left out "
                               "(the viewer and Nuke compute them)");
    }
    if (data.shortFaces > 0) {
        rep.Warn("import", Count(data.shortFaces) + " polygon(s) with fewer than 3 corners left out");
    }
    if (data.brokenFaces > 0) {
        rep.Warn("import", Count(data.brokenFaces) + " polygon(s) pointing at vertices the file does not have left out");
    }
    if (data.lines > 0 || data.points > 0) {
        rep.Warn("import", Count(data.lines) + " line(s) and " + Count(data.points) +
                               " point(s) left out - USDprep reads polygon meshes");
    }
    if (data.freeform > 0) {
        rep.Warn("import", "free-form curves and surfaces left out - USDprep reads polygon meshes");
    }

    // ----- save ---------------------------------------------------------------------
    if (progress) progress->fraction = 0.9f;
    const fs::path target = PathOf(usdPath);
    fs::create_directories(target.parent_path(), ec);
    if (!stage->GetRootLayer()->Export(usdPath)) {
        rep.Fail("cannot write " + usdPath);
        return rep;
    }
    rep.outputSizeBytes = fs::file_size(target, ec);
    rep.after.prims = data.parts.size();
    rep.after.meshes = data.parts.size();
    rep.after.materials = materialCount;
    rep.ok = true;
    if (progress) progress->fraction = 1.0f;
    return rep;
}

}  // namespace usdprep
