# M0 Spike — Working Log

**Started:** 2026-09-17
**Goal:** Build usdtweak on Windows; validate extract→flatten→package on real scenes; prep for Nuke 16.0/17 load test.

## Environment set up (all per-user, no admin)

| Tool | Version | Location / notes |
|---|---|---|
| pixi | 0.81.0 | `C:\Users\Petr\bin\pixi.exe` (bin added to user PATH) |
| git-lfs | 3.8.0 | `C:\Users\Petr\bin\git-lfs.exe` |
| CMake (portable) | 4.4.3 | `C:\Users\Petr\bin\cmake\` |
| OpenUSD runtime | 25.11 (conda-forge) | pixi env `third_party\usdtools` (`usdcat`, `usdzip`, `usdview`, python+pxr) |
| VS Build Tools | 2019, MSVC 14.29.30133 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools` |

Gotcha: Git Bash mangles prim paths (`/Kitchen_set/...` → `C:/Program Files/Git/...`). Run USD tools with `MSYS_NO_PATHCONV=1`.

## Test scenes

| Scene | Source | Size | Purpose |
|---|---|---|---|
| Kitchen Set (Pixar 26.08 copy) | `openusd.org/files/Kitchen_set.zip` | 5.3 MB unpacked | extraction/instancing tests; **no Material prims — displayColor only** (Maya export) |
| ALab (Netflix/Animal Logic via DPEL) | github `DigitalProductionExampleLibrary/ALab` + optional packs | repo ~1 GB | production-grade: payloads, variants, instancing, materials, looks |
| usd-wg/assets (ASWF) | github `usd-wg/assets` | 524 MB | small unit-style assets, StandardShaderBall full asset |

**ALab discovery:** the git repo alone is hollow — mesh `.usd` files are placeholders ("replaced with the techvar_assets package"). Optional packs (via `install_optional_packages.py`):
- techvars (geometry) **9.6 GB** — downloading
- procedurals (baked fur/fabric) **19.5 GB** — deferred
- textures **76.7 GB** — deferred (only needed for rendering-faithful tests)
- cameras **0.5 MB** — installed

## Extraction pipeline test (Kitchen Set) ✅

```
usdcat --flatten --mask /Kitchen_set/Props_grp/North_grp/StoveArea_grp/TeaKettle_1 Kitchen_set.usd -o teakettle.usda
usdcat teakettle.usda -o teakettle.usdc
usdzip teakettle.usdz teakettle.usdc
```

- Whole scene (zipped): 2.7 MB → extracted TeaKettle: **193 KB usda → 32.6 KB usdc → 32.8 KB usdz**
- Result opens standalone: 13 prims, 6 meshes, ancestors preserved, `defaultPrim=/Kitchen_set` inherited (not yet set to the asset — curation TODO for the tool).
- Extraction takes < 1 s. Flatten of whole scene: 42 MB usda.

## ALab composition anatomy (why unoptimized USD hurts Nuke)

```
entry.usda (subLayers: baked_procedurals/main, trailer_cameras/main, light_pre_input)
└─ /root (payload)
   ├─ /root/alab_set01 → 8 assemblies (lab_structure01, lab_projector01, …)
   │    └─ assembly → references entity/<name>/<name>.usda (layerOffset 0,1)
   │         └─ instanceable children → entity/<decor>/ with subLayers:
   │              ├─ surfacing → PAYLOAD look (materials, bindings)
   │              ├─ modelling → variantSet "geo" (base/deform_high/render_high…) → PAYLOAD meshes
   │              └─ preview (cards)
   ├─ /root/remi, /root/stoat (characters), /root/camera01, dmp skydome, audio prims
```

- Mask+flatten of `/root/alab_set01/lab_projector01_0001` on the hollow repo: geometry didn't appear (payload files are placeholders) — retest after techvars install.
- **Product insight confirmed:** raw `usdcat --mask --flatten` is not enough for Nuke-ready output: materials/textures live behind payloads in `fragment/look/...`, referenced by paths that break when subtree is extracted; `defaultPrim`, `purpose`, texture relinking must be curated by our tool.

## usdtweak build ✅ (done)

- Cloned `develop` @ 7332fce ("fix patch and build issue on windows").
- `windows-build.ps1` hard-requires **VS2022**; machine has VS2019 BuildTools (MSVC 14.29) → replicated its steps manually with `-G "Visual Studio 16 2019"`.
- **Attempt 1 — NVIDIA USD 25.08 (464 MB):** configure OK (after synthesizing TBB/OpenSubdiv/Imath cmake configs exactly like the script; the 25.08 pkg does ship osdCPU/osdGPU libs). **Compile FAILS**: `develop` now targets USD ≥25.11/26.x (`pxr/usd/sdf/textParserUtils.h` missing, `SdfAttributeSpec::HasSpline` absent in 25.08). The ps1/NVIDIA pin is stale vs develop. Lesson: don't trust the script's pinned USD; check `pixi.toml`/Building.md.
  - Also: `cmd | tail` swallows the build's exit code (pipeline status) — always log to file and echo `$?` separately.
- **Attempt 2 — conda-forge openusd 25.11 (already in pixi env): SUCCESS.**
  - `cmake -S . -B build-conda -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=<env>/Library -DCMAKE_PREFIX_PATH=<env>/Library/lib/cmake` + conda python paths (`<env>/python.exe`, `<env>/libs/python314.lib`, `<env>/include`, version 3.14 — pxrConfig requires exact-version Python).
  - MSVC 14.29 links cleanly against the clang-cl-built conda import libs.
  - Result: `build-conda/RelWithDebInfo/usdtweak.exe` (10.7 MB), exit 0.
- **Run:** launched via `pixi run --manifest-path third_party/usdtools/pixi.toml <exe> testdata/.../Kitchen_set.usd` (pixi activates the env → DLLs on PATH). App started fine:
  ```
  NVIDIA GeForce RTX 5090 / OpenGL 4.6.0 NVIDIA 591.86 / GLSL 4.60 / USD 2511
  ```
  → first data point for the §5.6 qualified-driver table: **RTX 5090, driver 591.86, OpenGL 4.6 — WORKS**.
- GPU/driver check confirmed working in practice (no Mesa fallback needed on the dev machine).

## ALab material-aware extraction test ✅ (done)

After installing techvars (see gotcha below): full stage = **11,887 prims / 1,522 meshes / 1,267 materials**, loads in 0.2 s.

```
usdcat --flatten --mask /root/alab_set01/lab_projector01_0001 entry.usda -o alab_projector.usda   # 1.86 MB
usdcat alab_projector.usda -o alab_projector.usdc                                                  # 394 KB
usdzip --asset alab_projector.usdc alab_projector_asset.usdz                                       # 4.8 MB w/ textures
```

- Materials in ALab entities are nested **inside** the extracted subtree (`…/MATERIAL/usd_full`, `usd_preview`, `usd_preview_proxy`) → mask+flatten **preserves meshes, materials, bindings, both shader networks** (full renderer + UsdPreviewSurface preview). Structure is Nuke-friendly by design.
- `usdzip --asset <in.usdc> <out.usdz>` (asset path is an OPTION value) localizes + packages the **UDIM tiles** (1001.exr set) and preview jpgs; two warnings for preview jpg refs that were authored relative — files still included.

**Product backlog confirmed (what usdprep must cururate):**
1. Flatten keeps **absolute texture paths** (Windows paths into the source tree) → relink/pack (usdzip --asset does it, but with warnings; our tool should do it deliberately).
2. `instanceable = true` survives flatten → standalone single-instance assets should be **de-instanced** (tools that don't traverse instance proxies see an empty subtree — bit us in our own roundtrip check).
3. `defaultPrim` not set on output → set to the asset prim.
4. Preview **cards** (0/ textures, `preview` purpose) get packaged → purpose-based pruning.

### Techvars install gotchas
- The 9.6 GB zip contains `techvar_assets/fragment/…`; correct install = merge into `ALab/fragment/` **overriding** placeholders: `tar -xf zip --strip-components=2 -C ALab/fragment`.
- The official `install_optional_packages.py` has a **Windows bug**: `_unzip` filters members by `zip_file_folder_name + os.sep` (`fragment\`), which never matches zip paths → writes nothing yet reports success.
- Placeholder mesh files (`#usda 1.0 … placeholder layer`) are committed to the repo on purpose; real crate files start with `PXR-USDC`.

## Status at end of session

| M0 item | State |
|---|---|
| usdtweak built on Windows | ✅ (conda USD 25.11, VS2019, exe 10.7 MB, running) |
| Test scenes | ✅ Kitchen Set, ALab + techvars + cameras, usd-wg/assets |
| mask→flatten→usdc→usdz pipeline | ✅ on both scenes, materials/UDIM textures survive |
| Addon mechanism understood | ✅ (doc/Addons.md) |
| Nuke 16.0 + 17 load test | ⏳ user-side: try `testdata/out/teakettle.usdz` and `testdata/out/alab_projector_asset.usdz` |
| Mesa fallback decision | ✅ not needed on dev machine (RTX 5090); revisit for release |

## Addon mechanism (studied, for the future Prep panel)

`doc/Addons.md` is a complete addon guide: drop a folder in `src/addons/<Name>/` with a one-line CMakeLists (`usdtweak_add_addon`), register via `TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, Tag)`, talk to the editor only via stable `src/addons/Api.h` (GetCurrentStage, selection, OpenStage, SearchPrimsByName, settings, modal dialogs, notices). Mutations must go through `ExecuteAfterDraw` for undo. CMake auto-globs addon folders (CONFIGURE_DEPENDS) — no host file edits needed. Three example addons ship (LauncherBar, ShaderRegistryInspector, StormPlayblast).
