#pragma once

#include <string>
#include <vector>

#include <usdprep/Extract.h>
#include <usdprep/Prune.h>

namespace usdprep {

// A recipe is every decision a run makes, independent of which operation
// runs and of which file it runs on. The shipped presets are recipes; a
// recipe file is the same thing written as JSON, so a studio can hand a
// prepared one to the farm.
struct Recipe {
    std::string name = "custom";
    std::string description;
    bool deinstance = true;
    bool setDefaultPrim = true;
    bool relinkTextures = true;
    // Categories removed from the result (Select's vocabulary).
    std::vector<std::string> dropTypes;
    std::vector<std::string> dropPurposes;
    // The material diet: "preview" | "full" | "all", plus the two cleanups
    // (see ExtractOptions).
    std::string materialPurpose = "all";
    bool stripRenderContexts = true;
    bool stripUnusedMaterials = true;
    bool stripDrawModeCards = true;
    // Nuke does not read UDIM tile sets: stitch each multi-tile set into one
    // texture and squeeze the UVs into it inside the material.
    bool udimAtlas = true;
    // Whatever is left that Nuke cannot read is converted or replaced and
    // reported (NUKE_COMPAT.md): kept guide/proxy geometry is hidden,
    // MaterialX outputs next to a standard surface go, meshes are unbound
    // from materials Nuke cannot render, unsupported lights become axes.
    bool nukeCompat = true;
    // Textures above this many pixels on the longer side are scaled down
    // in the output; 0 leaves them alone.
    int maxTextureSize = 0;
    // Decimate meshes to this share of their triangles; 0 = as they are.
    // Opt-in: no preset turns it on.
    double simplifyRatio = 0.0;
    // Animation: "all" | "range" | "static", with the frames as in
    // ExtractOptions (kStageFrame = the stage's own).
    std::string animation = "all";
    double frameStart = kStageFrame;
    double frameEnd = kStageFrame;
    double staticFrame = kStageFrame;
};

// Names of the built-in presets, in the order they are offered.
std::vector<std::string> PresetNames();

// Fill `recipe` from a built-in preset. False = no such preset.
bool GetPreset(const std::string& name, Recipe* recipe);

// Read a recipe from a JSON file. Unknown keys are reported through
// `warnings` (when given) but do not fail the load, so a recipe written
// for a later version still runs here. False = the file could not be
// used, with the reason in `error`.
bool LoadRecipe(const std::string& path, Recipe* recipe, std::string* error,
                std::vector<std::string>* warnings = nullptr);

// The JSON form of a recipe — what LoadRecipe reads, and what the CLI
// prints so a preset can be used as a starting point.
std::string RecipeToJson(const Recipe& recipe);

void ApplyRecipe(const Recipe& recipe, ExtractOptions* options);
void ApplyRecipe(const Recipe& recipe, PruneOptions* options);

}  // namespace usdprep
