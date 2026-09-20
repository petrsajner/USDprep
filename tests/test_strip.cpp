// The material diet: preview vs full bindings, render-context outputs,
// unused materials - and the textures that go with them.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>

#include <usdprep/Extract.h>
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

struct Output {
    std::vector<std::string> prims;     // every prim path
    std::vector<std::string> textures;  // every asset path, file name only
    std::vector<std::string> bindingsOnPanelA;  // "rel-name -> target"
};

Output Inspect(const std::string& stagePath) {
    Output out;
    const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(stagePath);
    if (!stage) return out;
    for (const pxr::UsdPrim& prim : stage->Traverse()) {
        out.prims.push_back(prim.GetPath().GetString());
        for (const pxr::UsdAttribute& attr : prim.GetAttributes()) {
            if (attr.GetTypeName() != pxr::SdfValueTypeNames->Asset) continue;
            pxr::SdfAssetPath value;
            if (attr.Get(&value) && !value.GetAssetPath().empty()) {
                out.textures.push_back(fs::path(value.GetAssetPath()).filename().string());
            }
        }
        if (prim.GetName() == "PanelA") {
            for (const pxr::UsdRelationship& rel : prim.GetRelationships()) {
                pxr::SdfPathVector targets;
                rel.GetTargets(&targets);
                for (const pxr::SdfPath& t : targets) {
                    out.bindingsOnPanelA.push_back(rel.GetName().GetString() + " -> " + t.GetString());
                }
            }
        }
    }
    return out;
}

bool Has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

usdprep::Report Export(const std::string& scene, const fs::path& outPath, const std::string& purpose,
                       bool contexts, bool unused) {
    usdprep::ExtractOptions options;
    options.primPaths = {"/Root"};
    options.outputPath = outPath.string();
    options.materialPurpose = purpose;
    options.stripRenderContexts = contexts;
    options.stripUnusedMaterials = unused;
    return usdprep::ExtractPrims(scene, options);
}

}  // namespace

int main() {
    const std::string scene = FIXTURE_DIR "/materials_scene.usda";
    const fs::path outDir = fs::current_path() / "strip_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    // --- the Nuke case: light material, no renderer outputs, no orphans ---
    {
        const usdprep::Report rep = Export(scene, outDir / "preview.usdc", "preview", true, true);
        CHECK(rep.ok);
        const Output out = Inspect((outDir / "preview.usdc").string());
        CHECK(Has(out.prims, "/Root/Looks/Light"));
        CHECK(Has(out.prims, "/Root/Looks/Multi"));
        CHECK(!Has(out.prims, "/Root/Looks/Hero"));    // the full-quality one, unbound now
        CHECK(!Has(out.prims, "/Root/Looks/Orphan"));  // never bound
        CHECK(!Has(out.prims, "/Root/Looks/Multi/Arnold"));     // only the arnold output reached it
        CHECK(!Has(out.prims, "/Root/Looks/Multi/ArnoldTex"));
        CHECK(Has(out.prims, "/Root/Looks/Multi/Preview"));
        CHECK(rep.after.materials == 2);
        // one binding, for every purpose, to the light material
        CHECK(out.bindingsOnPanelA.size() == 1);
        CHECK(Has(out.bindingsOnPanelA, "material:binding -> /Root/Looks/Light"));
        // the textures went with the shaders
        CHECK(out.textures.size() == 1);
        CHECK(Has(out.textures, "tile.1001.png"));
        bool reported = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "materials") reported = true;
        }
        CHECK(reported);
    }

    // --- the hero case: full material instead ---
    {
        const usdprep::Report rep = Export(scene, outDir / "full.usdc", "full", true, true);
        CHECK(rep.ok);
        const Output out = Inspect((outDir / "full.usdc").string());
        CHECK(Has(out.prims, "/Root/Looks/Hero"));
        CHECK(!Has(out.prims, "/Root/Looks/Light"));
        CHECK(Has(out.bindingsOnPanelA, "material:binding -> /Root/Looks/Hero"));
        CHECK(out.textures.size() == 1);
        CHECK(Has(out.textures, "checker.png"));
    }

    // --- keep both purposes, still clean up ---
    {
        const usdprep::Report rep = Export(scene, outDir / "all.usdc", "all", true, true);
        CHECK(rep.ok);
        const Output out = Inspect((outDir / "all.usdc").string());
        CHECK(Has(out.prims, "/Root/Looks/Hero"));
        CHECK(Has(out.prims, "/Root/Looks/Light"));
        CHECK(!Has(out.prims, "/Root/Looks/Orphan"));
        CHECK(!Has(out.prims, "/Root/Looks/Multi/Arnold"));
        CHECK(out.bindingsOnPanelA.size() == 2);  // full and preview, untouched
        CHECK(out.textures.size() == 2);          // checker + tile.1001; the arnold one is gone
    }

    // --- the raw case: nothing touched ---
    {
        const usdprep::Report rep = Export(scene, outDir / "raw.usdc", "all", false, false);
        CHECK(rep.ok);
        const Output out = Inspect((outDir / "raw.usdc").string());
        CHECK(Has(out.prims, "/Root/Looks/Orphan"));
        CHECK(Has(out.prims, "/Root/Looks/Multi/Arnold"));
        CHECK(rep.after.materials == 4);
        CHECK(out.textures.size() == 3);
    }

    // --- the presets say what they mean ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.materialPurpose == "preview");
        CHECK(nuke.stripRenderContexts);
        CHECK(nuke.stripUnusedMaterials);
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(raw.materialPurpose == "all");
        CHECK(!raw.stripRenderContexts);
        CHECK(!raw.stripUnusedMaterials);
    }

    if (failures == 0) std::printf("test_strip: OK\n");
    return failures == 0 ? 0 : 1;
}
