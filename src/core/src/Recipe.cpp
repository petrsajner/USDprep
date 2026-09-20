#include <usdprep/Recipe.h>

#include <cmath>
#include <fstream>
#include <sstream>

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>

namespace usdprep {

namespace {

using namespace pxr;

// The shipped recipes. "nuke" answers "just give me something I can drop
// into a comp"; "raw" is the escape hatch that curates nothing.
const Recipe& NukePreset() {
    static const Recipe recipe = [] {
        Recipe r;
        r.name = "nuke";
        r.description =
            "Nuke-ready: one self-contained asset, plain prims, no guide or "
            "proxy geometry. Cameras and lights are kept.";
        r.deinstance = true;
        r.setDefaultPrim = true;
        r.relinkTextures = true;
        r.dropPurposes = {"guide", "proxy"};
        r.materialPurpose = "preview";
        r.stripRenderContexts = true;
        r.stripUnusedMaterials = true;
        r.stripDrawModeCards = true;
        r.maxTextureSize = 4096;
        r.animation = "range";
        return r;
    }();
    return recipe;
}

const Recipe& RawPreset() {
    static const Recipe recipe = [] {
        Recipe r;
        r.name = "raw";
        r.description =
            "Flatten only: keep instancing, keep every purpose, leave texture "
            "paths alone. The closest thing to usdcat --flatten.";
        r.deinstance = false;
        r.setDefaultPrim = false;
        r.relinkTextures = false;
        r.materialPurpose = "all";
        r.stripRenderContexts = false;
        r.stripUnusedMaterials = false;
        r.stripDrawModeCards = false;
        r.maxTextureSize = 0;
        r.animation = "all";
        return r;
    }();
    return recipe;
}

bool ReadBool(const JsValue& value, bool* out, const std::string& key,
              std::string* error) {
    if (!value.IsBool()) {
        *error = "'" + key + "' must be true or false";
        return false;
    }
    *out = value.GetBool();
    return true;
}

bool ReadStringArray(const JsValue& value, std::vector<std::string>* out,
                     const std::string& key, std::string* error) {
    if (!value.IsArray()) {
        *error = "'" + key + "' must be a list of strings";
        return false;
    }
    out->clear();
    for (const JsValue& entry : value.GetJsArray()) {
        if (!entry.IsString()) {
            *error = "'" + key + "' must contain strings only";
            return false;
        }
        out->push_back(entry.GetString());
    }
    return true;
}

std::string JsonEscape(const std::string& s) {
    static const char kBackslash = '\\';
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == kBackslash) {
            out += kBackslash;
            out += c;
        } else if (c == '\n') {
            out += kBackslash;
            out += 'n';
        } else {
            out += c;
        }
    }
    return out;
}

std::string JsonStringList(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        out += (i == 0 ? "\"" : ", \"") + JsonEscape(values[i]) + "\"";
    }
    return out + "]";
}

}  // namespace

std::vector<std::string> PresetNames() { return {"nuke", "raw"}; }

bool GetPreset(const std::string& name, Recipe* recipe) {
    if (name == "nuke") {
        *recipe = NukePreset();
        return true;
    }
    if (name == "raw") {
        *recipe = RawPreset();
        return true;
    }
    return false;
}

bool LoadRecipe(const std::string& path, Recipe* recipe, std::string* error,
                std::vector<std::string>* warnings) {
    error->clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot read recipe file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    JsParseError parseError;
    const JsValue parsed = JsParseString(buffer.str(), &parseError);
    if (!parsed.IsObject()) {
        *error = parseError.reason.empty()
                     ? "recipe " + path + " must contain a JSON object"
                     : "recipe " + path + " is not valid JSON (line " +
                           std::to_string(parseError.line) + ": " +
                           parseError.reason + ")";
        return false;
    }

    // A recipe starts from the shipped defaults and overrides what it
    // mentions, so a two-line recipe file is a valid recipe.
    Recipe loaded;

    // "preset" is the base the rest of the file overrides, so it has to be
    // applied before the other keys — the object hands them over sorted by
    // name, which would otherwise let "dropTypes" lose to a later "preset".
    const JsObject& object = parsed.GetJsObject();
    const auto presetEntry = object.find("preset");
    if (presetEntry != object.end()) {
        const JsValue& value = presetEntry->second;
        if (!value.IsString() || !GetPreset(value.GetString(), &loaded)) {
            *error = "'preset' must name a built-in preset";
            return false;
        }
        loaded.name = "custom";
    }

    for (const auto& entry : object) {
        const std::string& key = entry.first;
        const JsValue& value = entry.second;
        bool ok = true;
        if (key == "name" || key == "description") {
            if (!value.IsString()) {
                *error = "'" + key + "' must be a string";
                return false;
            }
            (key == "name" ? loaded.name : loaded.description) = value.GetString();
        } else if (key == "preset") {
            // already applied above, as the base for everything else
        } else if (key == "deinstance") {
            ok = ReadBool(value, &loaded.deinstance, key, error);
        } else if (key == "setDefaultPrim") {
            ok = ReadBool(value, &loaded.setDefaultPrim, key, error);
        } else if (key == "relinkTextures") {
            ok = ReadBool(value, &loaded.relinkTextures, key, error);
        } else if (key == "dropTypes") {
            ok = ReadStringArray(value, &loaded.dropTypes, key, error);
        } else if (key == "dropPurposes") {
            ok = ReadStringArray(value, &loaded.dropPurposes, key, error);
        } else if (key == "materialPurpose") {
            const std::string choice = value.IsString() ? value.GetString() : "";
            if (choice != "preview" && choice != "full" && choice != "all") {
                *error = "'materialPurpose' must be \"preview\", \"full\" or \"all\"";
                return false;
            }
            loaded.materialPurpose = choice;
        } else if (key == "stripRenderContexts") {
            ok = ReadBool(value, &loaded.stripRenderContexts, key, error);
        } else if (key == "stripUnusedMaterials") {
            ok = ReadBool(value, &loaded.stripUnusedMaterials, key, error);
        } else if (key == "stripDrawModeCards") {
            ok = ReadBool(value, &loaded.stripDrawModeCards, key, error);
        } else if (key == "maxTextureSize") {
            if (!value.IsInt() || value.GetInt() < 0) {
                *error = "'maxTextureSize' must be a whole number of pixels (0 = no cap)";
                return false;
            }
            loaded.maxTextureSize = value.GetInt();
        } else if (key == "simplifyRatio") {
            const double ratio = value.IsInt() ? static_cast<double>(value.GetInt64())
                                 : value.IsReal() ? value.GetReal() : -1.0;
            if (ratio < 0.0 || ratio >= 1.0) {
                *error = "'simplifyRatio' must be between 0 (as is) and 1, e.g. 0.25";
                return false;
            }
            loaded.simplifyRatio = ratio;
        } else if (key == "animation") {
            const std::string choice = value.IsString() ? value.GetString() : "";
            if (choice != "all" && choice != "range" && choice != "static") {
                *error = "'animation' must be \"all\", \"range\" or \"static\"";
                return false;
            }
            loaded.animation = choice;
        } else if (key == "frameStart" || key == "frameEnd" || key == "staticFrame") {
            if (!value.IsReal() && !value.IsInt()) {
                *error = "'" + key + "' must be a number";
                return false;
            }
            const double frame = value.IsInt() ? static_cast<double>(value.GetInt64()) : value.GetReal();
            (key == "frameStart" ? loaded.frameStart
             : key == "frameEnd" ? loaded.frameEnd
                                 : loaded.staticFrame) = frame;
        } else if (warnings) {
            warnings->push_back("recipe key '" + key +
                                "' is not understood by this version and was ignored");
        }
        if (!ok) return false;
    }
    *recipe = loaded;
    return true;
}

std::string RecipeToJson(const Recipe& recipe) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"name\": \"" << JsonEscape(recipe.name) << "\",\n";
    os << "  \"description\": \"" << JsonEscape(recipe.description) << "\",\n";
    os << "  \"deinstance\": " << (recipe.deinstance ? "true" : "false") << ",\n";
    os << "  \"setDefaultPrim\": " << (recipe.setDefaultPrim ? "true" : "false") << ",\n";
    os << "  \"relinkTextures\": " << (recipe.relinkTextures ? "true" : "false") << ",\n";
    os << "  \"dropTypes\": " << JsonStringList(recipe.dropTypes) << ",\n";
    os << "  \"dropPurposes\": " << JsonStringList(recipe.dropPurposes) << ",\n";
    os << "  \"materialPurpose\": \"" << JsonEscape(recipe.materialPurpose) << "\",\n";
    os << "  \"stripRenderContexts\": " << (recipe.stripRenderContexts ? "true" : "false") << ",\n";
    os << "  \"stripUnusedMaterials\": " << (recipe.stripUnusedMaterials ? "true" : "false") << ",\n";
    os << "  \"stripDrawModeCards\": " << (recipe.stripDrawModeCards ? "true" : "false") << ",\n";
    os << "  \"maxTextureSize\": " << recipe.maxTextureSize << ",\n";
    os << "  \"simplifyRatio\": " << recipe.simplifyRatio << ",\n";
    os << "  \"animation\": \"" << JsonEscape(recipe.animation) << "\"";
    // frames only when set: JSON has no way to say "the stage's own"
    if (!std::isnan(recipe.frameStart)) os << ",\n  \"frameStart\": " << recipe.frameStart;
    if (!std::isnan(recipe.frameEnd)) os << ",\n  \"frameEnd\": " << recipe.frameEnd;
    if (!std::isnan(recipe.staticFrame)) os << ",\n  \"staticFrame\": " << recipe.staticFrame;
    os << "\n}\n";
    return os.str();
}

void ApplyRecipe(const Recipe& recipe, ExtractOptions* options) {
    options->deinstance = recipe.deinstance;
    options->setDefaultPrim = recipe.setDefaultPrim;
    options->relinkTextures = recipe.relinkTextures;
    options->dropTypes = recipe.dropTypes;
    options->dropPurposes = recipe.dropPurposes;
    options->materialPurpose = recipe.materialPurpose;
    options->stripRenderContexts = recipe.stripRenderContexts;
    options->stripUnusedMaterials = recipe.stripUnusedMaterials;
    options->stripDrawModeCards = recipe.stripDrawModeCards;
    options->maxTextureSize = recipe.maxTextureSize;
    options->simplifyRatio = recipe.simplifyRatio;
    options->animation = recipe.animation;
    options->frameStart = recipe.frameStart;
    options->frameEnd = recipe.frameEnd;
    options->staticFrame = recipe.staticFrame;
}

void ApplyRecipe(const Recipe& recipe, PruneOptions* options) {
    options->deinstance = recipe.deinstance;
    options->setDefaultPrim = recipe.setDefaultPrim;
    options->relinkTextures = recipe.relinkTextures;
    options->dropTypes = recipe.dropTypes;
    options->dropPurposes = recipe.dropPurposes;
    options->materialPurpose = recipe.materialPurpose;
    options->stripRenderContexts = recipe.stripRenderContexts;
    options->stripUnusedMaterials = recipe.stripUnusedMaterials;
    options->stripDrawModeCards = recipe.stripDrawModeCards;
    options->maxTextureSize = recipe.maxTextureSize;
    options->simplifyRatio = recipe.simplifyRatio;
    options->animation = recipe.animation;
    options->frameStart = recipe.frameStart;
    options->frameEnd = recipe.frameEnd;
    options->staticFrame = recipe.staticFrame;
}

}  // namespace usdprep
