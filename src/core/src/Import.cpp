// Which importer reads which file - and nothing a reader throws reaches
// the caller: it becomes a failed import that says why.
#include <usdprep/Import.h>

#include <new>
#include <stdexcept>

#include "ImportShared.h"

namespace usdprep {

bool IsImportable(const std::string& path) {
    return importer::EndsWithNoCase(path, ".obj") || importer::EndsWithNoCase(path, ".abc") ||
           importer::EndsWithNoCase(path, ".fbx");
}

Report ImportToUsd(const std::string& inputPath, const std::string& usdPath, ImportProgress* progress) {
    Report rep;
    rep.inputPath = inputPath;
    rep.outputPath = usdPath;
    try {
        if (importer::EndsWithNoCase(inputPath, ".obj")) return importer::ImportObj(inputPath, usdPath, progress);
        if (importer::EndsWithNoCase(inputPath, ".abc")) return importer::ImportAbc(inputPath, usdPath, progress);
        if (importer::EndsWithNoCase(inputPath, ".fbx")) return importer::ImportFbx(inputPath, usdPath, progress);
        rep.Fail("USDprep converts .obj, .abc and .fbx files; this is none of them: " + inputPath);
    } catch (const std::bad_alloc&) {
        rep.Fail("not enough memory to convert " + inputPath);
    } catch (const std::exception& e) {
        rep.Fail("cannot convert " + inputPath + ": " + importer::ReadableError(e.what()));
    } catch (...) {
        rep.Fail("cannot convert " + inputPath);
    }
    return rep;
}

}  // namespace usdprep
