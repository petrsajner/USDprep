#include <usdprep/Select.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdLux/lightAPI.h>

namespace usdprep {

namespace {

using namespace pxr;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Wildcard match ('*' = any run, '?' = one character) with backtracking.
// Iterative on purpose: the pattern comes from a text field and a recursive
// matcher blows the stack on adversarial input like "*a*a*a*a*a*b".
bool GlobMatch(const std::string& text, const std::string& pattern) {
    size_t t = 0, p = 0, starP = std::string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++t;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool NameMatches(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return true;
    const std::string lowerName = ToLower(name);
    const std::string lowerPattern = ToLower(pattern);
    if (lowerPattern.find_first_of("*?") == std::string::npos) {
        return lowerName.find(lowerPattern) != std::string::npos;
    }
    return GlobMatch(lowerName, lowerPattern);
}

bool TypeMatches(const UsdPrim& prim, const std::vector<std::string>& types) {
    if (types.empty()) return true;
    const std::string typeName = ToLower(prim.GetTypeName().GetString());
    for (const std::string& wanted : types) {
        const std::string w = ToLower(wanted);
        if (w == "light") {
            // Family, not a type: SphereLight, DomeLight, a mesh with
            // LightAPI applied — they all carry UsdLuxLightAPI.
            if (prim.HasAPI<UsdLuxLightAPI>()) return true;
        } else if (w == typeName) {
            return true;
        }
    }
    return false;
}

bool PurposeMatches(const UsdPrim& prim, const std::vector<std::string>& purposes) {
    if (purposes.empty()) return true;
    const UsdGeomImageable imageable(prim);
    if (!imageable) return false;
    const std::string purpose = imageable.ComputePurpose().GetString();
    for (const std::string& wanted : purposes) {
        if (ToLower(wanted) == purpose) return true;
    }
    return false;
}

}  // namespace

SelectResult SelectPrims(const UsdStageRefPtr& stage, const SelectOptions& options) {
    SelectResult result;
    if (!stage) {
        result.error = "no stage to search";
        return result;
    }

    std::vector<UsdPrim> searchRoots;
    if (options.roots.empty()) {
        searchRoots.push_back(stage->GetPseudoRoot());
    } else {
        for (const std::string& s : options.roots) {
            const SdfPath path(s);
            if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
                result.error = "not an absolute prim path: '" + s + "'";
                return result;
            }
            const UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim) {
                result.error = "prim does not exist: '" + s + "'";
                return result;
            }
            searchRoots.push_back(prim);
        }
    }

    std::set<std::string> matched;  // also dedupes overlapping search roots
    for (const UsdPrim& root : searchRoots) {
        for (const UsdPrim& prim :
             UsdPrimRange(root, UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            if (prim.IsPseudoRoot()) continue;
            ++result.visited;
            if (!TypeMatches(prim, options.types)) continue;
            if (!NameMatches(prim.GetName().GetString(), options.namePattern)) continue;
            if (!PurposeMatches(prim, options.purposes)) continue;
            const std::string path = prim.GetPath().GetAsString();
            if (matched.insert(path).second) result.paths.push_back(path);
        }
    }

    if (options.topmostOnly) {
        std::vector<std::string> topmost;
        for (const std::string& s : result.paths) {
            bool covered = false;
            for (SdfPath p = SdfPath(s).GetParentPath(); p.GetPathElementCount() >= 1;
                 p = p.GetParentPath()) {
                if (matched.count(p.GetAsString())) {
                    covered = true;
                    break;
                }
            }
            if (!covered) topmost.push_back(s);
        }
        result.paths.swap(topmost);
    }

    return result;
}

SelectResult SelectPrims(const std::string& inputPath, const SelectOptions& options) {
    const UsdStageRefPtr stage = UsdStage::Open(inputPath, UsdStage::LoadAll);
    if (!stage) {
        SelectResult result;
        result.error = "cannot open stage: " + inputPath;
        return result;
    }
    return SelectPrims(stage, options);
}

}  // namespace usdprep
