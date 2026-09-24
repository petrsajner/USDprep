// The bottom of the panel: what gets exported, with which recipe, to
// where — and the button.
#pragma once

#include <functional>
#include <memory>
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

    // A click on an entry of the export list: "show me that one" — the
    // tree and the 3D view select it. Set by the panel that owns both.
    std::function<void(const pxr::SdfPath&)> onPick;

private:
    void DrawList(const std::vector<pxr::SdfPath>& selectionRoots);
    void DrawPreset();
    void DrawDestination(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);
    void DrawAdvanced();
    void DrawRun(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);
    void ChoosePreset(int choice, const std::string& recipePath);
    void Run(const pxr::UsdStageRefPtr& stage, const std::vector<pxr::SdfPath>& targets);
    void DrawProgress();
    void Finish();

    // The export runs in the background: the window stays alive, shows how
    // far it is and can stop it. Null when nothing runs.
    struct Job;
    std::shared_ptr<Job> _job;

    std::vector<pxr::SdfPath> _list;

    // Presets in PresetNames() order, then one more entry: a recipe file.
    int _presetChoice = -1;
    std::string _recipePath;
    usdprep::Recipe _recipe;
    std::string _recipeProblem;
    bool _deinstance = true;
    bool _setDefaultPrim = true;
    bool _relinkTextures = true;
    int _materials = 0;  // 0 = preview (light), 1 = full (hero), 2 = all
    int _animation = 1;  // 0 = everything, 1 = the shot range, 2 = one frame
    int _textureCap = 0;  // index into the cap choices (0 = no cap)
    int _geometry = 0;    // stop on the geometry slider: 0 = as it is ... 5 = a hundredth of the polygons
    // Every reduction is a switch of its own, whatever the preset said.
    bool _includeLights = false;  // off by default: a file's lights darken Nuke's render
    bool _dropGuideProxy = false;
    bool _stripRenderContexts = false;
    bool _stripUnusedMaterials = false;
    bool _stripCards = false;
    double _staticFrame = 0.0;
    bool _staticFrameSet = false;  // typed by the artist, else the scene's start

    char _outputPath[512] = "";
    int _format = 0;  // 0 = .usdc (current 3D system); classic 3D, older Nuke: 1 = .abc, 2 = .obj
    double _sceneStart = 0.0;  // the scene's frame range, for the labels
    double _sceneEnd = 0.0;
    pxr::UsdStageRefPtr _suggestedFor;
    std::string _suggestedName;  // the object the current suggestion is named after
    bool _pathEdited = false;    // the artist typed or browsed: stop suggesting

    bool _resultOk = false;
    bool _resultCancelled = false;  // stopped by the artist: said plainly, not as an error
    std::string _resultLine;
    std::string _report;
    bool _showReport = false;
    float _lastHeight = 0.0f;
};

}  // namespace usdprep_addon
