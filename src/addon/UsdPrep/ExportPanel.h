// The bottom of the panel: what gets exported, with which recipe, to
// where — and the button.
#pragma once

#include <string>
#include <vector>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <usdprep/Recipe.h>

namespace usdprep_addon {

class ExportPanel {
public:
    // `selectionRoots` is what Export takes while the export list is
    // empty — the one-object case stays one click.
    void Draw(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& selectionRoots);

    // Height of the section as drawn last frame; the tree above reserves it.
    float LastHeight() const { return _lastHeight; }

private:
    void DrawList(const std::vector<pxr::SdfPath>& selectionRoots);
    void DrawPreset();
    void DrawDestination(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);
    void DrawAdvanced();
    void DrawRun(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);
    void ChoosePreset(int choice, const std::string& recipePath);
    void Run(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);

    std::vector<pxr::SdfPath> _list;

    // Presets in PresetNames() order, then one more entry: a recipe file.
    int _presetChoice = -1;
    std::string _recipePath;
    usdprep::Recipe _recipe;
    std::string _recipeProblem;
    bool _deinstance = true;
    bool _setDefaultPrim = true;
    bool _relinkTextures = true;

    char _outputPath[512] = "";
    int _format = 0;  // 0 = .usdz package, 1 = .usdc layer
    pxr::UsdStageRefPtr _suggestedFor;
    std::string _suggestedName;  // the object the current suggestion is named after
    bool _pathEdited = false;    // the artist typed or browsed: stop suggesting

    bool _resultOk = false;
    std::string _resultLine;
    std::string _report;
    bool _showReport = false;
    float _lastHeight = 0.0f;
};

}  // namespace usdprep_addon
