// Opening scenes, the USDprep way.
//
// File > Open is the operating system's own dialog (network drives, DFS
// links, a pasted path - everything the file browser of the editor
// cannot do). A file USD cannot read (.obj) is converted to a temporary
// USD file in the background, with a progress bar, and that is opened -
// from then on it is a scene like any other. When a file cannot be
// opened, a window says so instead of nothing happening.
//
// The functions below are usdtweak's addon hooks (see UsdTweakAddon).
#pragma once

#include <string>

#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/usd/stage.h>

namespace usdprep_addon {

bool OnOpenDialog();                        // File > Open
bool OnOpenStage(std::string& path);        // before a stage is opened from a file
bool OnDropFile(const std::string& path);   // a file dropped on the window
void OnFrame();                             // progress and messages

// Conversions left behind by earlier sessions go (those still in use stay).
void ForgetOldImports();

// The file the artist opened: an imported scene's source, else the scene's
// own file. Empty for a scene that was never saved.
std::string SourcePathOf(const pxr::UsdStageRefPtr& stage);

// True for a scene USDprep converted on opening.
bool IsImported(const pxr::UsdStageRefPtr& stage);

// What the conversion said, one note per line ("warning: ..." first);
// empty for any other scene.
const std::string& ImportNotesOf(const pxr::UsdStageRefPtr& stage);

// Where a converted scan really sits when the 3D view shows it at the
// origin (see ShowAtOrigin in the .cpp); false for every other scene.
bool ShownAtOrigin(const pxr::UsdStageRefPtr& stage, pxr::GfVec3d* position);

}  // namespace usdprep_addon
