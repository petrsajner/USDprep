# M1 — core + CLI MVP: working log

**Started:** 2026-09-17 (same day M0 closed)

## Vertical slice delivered (v0.1.0)

`usdprep-core` (UI-free C++17) + `usdcut` CLI with Inspect / Extract /
Prune / Package, JSON + text reports, ctest suite (3 tests, all passing).

Validated on real scenes (all < 1 s):

| command | input | result |
|---|---|---|
| `usdcut inspect` ALab entry.usda | 324 B layer → 47,401 prims composed | 7,484 meshes, 5,561 materials, 26,306 shaders, 15,183 texture refs [.exr .jpg], 1,431 instances, frames 1004–1057 |
| `usdcut extract` projector assembly | 47k prims | 26 prims / 2 meshes / 3 materials / 14 shaders, de-instanced, defaultPrim authored, **4.56 MB usdz** with UDIM exr + preview jpgs localized |
| `usdcut prune --except` projector+camera | 47k prims | 38 prims / 4 meshes / 1 camera, 393 KB usdc |
| `usdcut extract` TeaKettle (Kitchen) | 2,742 prims | 31.9 KB usdz — matches the M0 manual pipeline byte-for-byte scale |

## Design decisions (v0)

- Authoring happens in-memory only (session layer / the temp output layer);
  the input file is never written to. defaultPrim is authored **post-export**
  on the flattened temp file via `UsdStage::SetDefaultPrim` (which always
  writes the stage's root layer — the temp, not the user's input).
- De-instancing is authored into the masked stage's session layer before
  `Export` (flattened output then contains plain prims).
- Prune keep-mode = masked extraction with several roots (same code path);
  drop-mode = flatten, reopen the single-layer output, `UsdStage::RemovePrim`
  per deduplicated root, save.
- usdz packaging = `UsdUtilsCreateNewUsdzPackage` on the flattened temp
  (localizes UDIM tiles; relative-refs warnings from USD are non-fatal —
  files still land in the package, verified in M0).
- No JSON dependency yet: hand-rolled writer for reports; recipes/presets
  (nlohmann/json) come with the recipe engine.

## Gotchas hit during M1 (worth remembering)

- pxr requires `find_package(OpenGL)` **before** `find_package(pxr)`
  (garch links `OpenGL::GL`).
- `UsdStage::SetDefaultPrim` returns void and always targets the root layer.
- Test/CLI executables need conda USD DLLs on PATH — and in Git Bash the
  PATH entries must be **MSYS-style** (`/c/...`); Windows-style `C:/...`
  entries get split at the drive-letter colon and silently never match
  (symptom: 0xc0000135 dialogs from ctest).
- Export of ALab emits `_CopyMetadata` warnings for unknown custom fields
  (`al_usdmaya_*`, `assettype`) — harmless, and exactly the junk the future
  Strip op will remove.
- `UsdPrimRange` default predicate hides instance content — use
  `UsdTraverseInstanceProxies(UsdPrimDefaultPredicate)` everywhere we count.

## Next in M1 (not yet done)

- Select op (filters by type/name/purpose) — needed by GUI later
- Golden-file tests on real-scene outputs (byte-stable usdc)
- Recipe/preset engine (JSON, `--preset nuke` wiring)
- Texture relink for non-usdz outputs (usdc folder layout)
