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

## M1 completed (v0.2.0, 2026-09-20)

The four items left open above are done.

| item | what shipped |
|---|---|
| **Select** | `usdprep::SelectPrims` (path or open-stage overload) filtering by type, name and resolved purpose, optionally under roots, optionally topmost-only; `usdcut select` prints paths on stdout, summary on stderr; `prune --drop-type/--drop-purpose` drops whole categories |
| **Texture relink** | non-package output copies dependencies into `<name>_textures` next to the file and repoints the paths (`UsdUtilsLocalizeAsset` + `UsdUtilsModifyAssetPaths`), UDIM tile sets included; `--no-relink` opts out |
| **Recipes/presets** | `Recipe` + presets `nuke` / `raw`, `--preset`, `--recipe file.json`, `usdcut presets [name]`; JSON via USD's own `pxr/base/js` (no new dependency); extract honours the recipe's drop filters |
| **Golden tests** | `test_golden`: byte-for-byte determinism (text + crate) plus committed path-normalized `.usda` and report goldens, regenerated with `USDPREP_UPDATE_GOLDEN=1` |

Suite is now 7 tests, all passing. Validated on ALab (47,401 prims):
`select --type light` finds 57 lights, `--purpose proxy,guide` 1,450
subtrees, whole-scene prune by purpose runs in ~16 s.

## Defects this work turned up (all fixed)

- **UDIM paths were silently lost.** Flatten anchors asset paths by
  resolving them; a `<UDIM>` template never resolves, so it survived as a
  relative path and pointed at nothing once the output moved — and the
  tiles were missing from `.usdz` packages too. Such paths are now
  anchored against the layer that authored them *before* the export, and
  lexically, because the resolver refuses to anchor what it cannot
  resolve.
- **Lights were counted by one concrete type** (`DistantLight`), so ALab
  reported 0 lights instead of 57. Now `prim.HasAPI<UsdLuxLightAPI>()`.
- **Prune could not delete inside instanced content** and failed the whole
  run; such matches are now kept with a warning (deleting them needs
  de-instancing).
- **defaultPrim pointed at `/Flattened_Prototype_1`** after a prune that
  kept instancing; it now prefers the input's default prim, then the
  first non-prototype top-level prim.
- **A recipe's `preset` key was applied last** (the JSON object hands keys
  over sorted by name) and wiped the values it is supposed to be the base
  for.
- The temp layer's file name became the root layer name inside a `.usdz`
  (`panel.usdz.usdprep-tmp.usdc`); the temp now lives in a scratch folder
  and carries the output's own name.

## Known gaps (carried forward)

- Preview **cards** are `model:cardTexture*` attributes on the prim
  itself, not a subtree, so their textures still travel into a package.
  Clearing them belongs to the Strip op (M3).
- Whole-scene prune with de-instancing on is a size trap: ALab's 1,431
  instances expand to a 767 MB layer. Fine for a single extracted asset,
  wrong default for a whole scene — needs a smarter preset decision
  (or a warning) before the GUI offers scene-wide prune.
- `instanceable = false` is authored explicitly in the output; harmless,
  but it is noise a Strip pass should clear.
- The CLI's default is still "no preset" (today's behaviour). The GUI
  defaulting to the Nuke preset is an M2 decision.
