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

    // --- draw-mode cards: the six textures a viewer's stand-in box would
    //     use go with the setup; the geometry stays ---
    {
        const std::string cards = FIXTURE_DIR "/cards_scene.usda";
        usdprep::ExtractOptions options;
        options.primPaths = {"/Root"};
        options.outputPath = (outDir / "cards.usdc").string();
        options.stripDrawModeCards = true;
        const usdprep::Report rep = usdprep::ExtractPrims(cards, options);
        CHECK(rep.ok);
        const Output out = Inspect((outDir / "cards.usdc").string());
        CHECK(out.textures.empty());
        CHECK(Has(out.prims, "/Root/Body"));
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "cards.usdc").string());
        // the applied schema still defines the attribute; nothing is authored any more
        CHECK(!stage->GetPrimAtPath(pxr::SdfPath("/Root"))
                   .GetAttribute(pxr::TfToken("model:drawMode"))
                   .HasAuthoredValue());
        bool reported = false;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action == "cards" && e.detail.find("6 card texture") != std::string::npos) reported = true;
        }
        CHECK(reported);

        options.outputPath = (outDir / "cards_kept.usdc").string();
        options.stripDrawModeCards = false;
        CHECK(usdprep::ExtractPrims(cards, options).ok);
        CHECK(Inspect((outDir / "cards_kept.usdc").string()).textures.size() == 6);
    }

    // --- only what Nuke reads: a hero material on a multi-tile UDIM set
    //     gives way to the light one; an ordinary hero material stays ---
    {
        const usdprep::Report rep =
            Export(FIXTURE_DIR "/udim_hero_scene.usda", outDir / "udim_hero.usdc", "full", true, true);
        CHECK(rep.ok);
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open((outDir / "udim_hero.usdc").string());
        const auto boundTo = [&](const char* prim) {
            pxr::SdfPathVector targets;
            stage->GetPrimAtPath(pxr::SdfPath(prim)).GetRelationship(pxr::TfToken("material:binding")).GetTargets(&targets);
            return targets.empty() ? std::string() : targets.front().GetString();
        };
        CHECK(boundTo("/Root/Geo/Tiled") == "/Root/Looks/Light");
        CHECK(boundTo("/Root/Geo/Plain") == "/Root/Looks/HeroPlain");
        CHECK(boundTo("/Root/Geo/Alone") == "/Root/Looks/HeroTiled");  // nothing to fall back to
        int fallbackWarnings = 0;
        int udimWarnings = 0;
        for (const usdprep::ReportEntry& e : rep.entries) {
            if (e.action != "nuke") continue;
            if (e.detail.find("light material instead") != std::string::npos) ++fallbackWarnings;
            if (e.detail.find("render black") != std::string::npos) ++udimWarnings;
        }
        CHECK(fallbackWarnings == 1);
        CHECK(udimWarnings == 1);  // Alone is still said out loud
    }

    // --- the presets say what they mean ---
    {
        usdprep::Recipe nuke;
        usdprep::GetPreset("nuke", &nuke);
        CHECK(nuke.materialPurpose == "preview");
        CHECK(nuke.stripRenderContexts);
        CHECK(nuke.stripUnusedMaterials);
        CHECK(nuke.stripDrawModeCards);
        usdprep::Recipe raw;
        usdprep::GetPreset("raw", &raw);
        CHECK(raw.materialPurpose == "all");
        CHECK(!raw.stripRenderContexts);
        CHECK(!raw.stripUnusedMaterials);
        CHECK(!raw.stripDrawModeCards);
    }

    if (failures == 0) std::printf("test_strip: OK\n");
    return failures == 0 ? 0 : 1;
}
