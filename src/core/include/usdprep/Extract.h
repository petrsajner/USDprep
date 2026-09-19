#pragma once

#include <string>
#include <vector>

#include <usdprep/Report.h>

namespace usdprep {

struct ExtractOptions {
    std::vector<std::string> primPaths;  // subtree roots to keep
    std::string outputPath;              // .usda / .usdc / .usdz
    bool deinstance = true;              // drop instanceable flags in output
    bool setDefaultPrim = true;          // author defaultPrim on the output
    bool relinkTextures = true;          // .usdc/.usda: copy textures next to
                                         // the output and repoint the paths
};

// Copy the given subtrees (with their ancestors and carried dependencies)
// into a new standalone, flattened file. The input file is never modified.
//
// v0 behavior notes (validated in M0):
// - Materials living inside the extracted subtrees survive; materials bound
//   from outside the mask are dropped together with their bindings' targets
//   (dependency curation is the next milestone).
// - .usdz output localizes referenced textures (incl. UDIM tiles) into the
//   package; .usdc/.usda output copies them into a "<name>_textures" folder
//   next to the file and rewrites the paths (relinkTextures).
Report ExtractPrims(const std::string& inputPath, const ExtractOptions& options);

}  // namespace usdprep
