// The file dialogs of the operating system (Windows): they know every
// drive the machine knows - network shares, DFS links, OneDrive - take a
// pasted path, remember their folder. Elsewhere they are not available
// and every call answers "cancelled".
#pragma once

#include <string>

namespace usdprep_addon {

bool NativeDialogsAvailable();

// "Save as..." for the export, offering the formats Nuke reads
// (0 = .usdc, 1 = .abc, 2 = .obj). False = cancelled.
bool NativeSaveDialog(const std::string& suggestedName, int format, std::string& outPath);

// "Open" for a recipe file (.json). False = cancelled.
bool NativeOpenRecipeDialog(std::string& outPath);

// "Open" for a scene: USD, or a file USDprep imports (.obj). False = cancelled.
bool NativeOpenSceneDialog(std::string& outPath);

}  // namespace usdprep_addon
