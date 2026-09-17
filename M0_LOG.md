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

## usdtweak build (in progress)

- Cloned `develop` @ 7332fce ("fix patch and build issue on windows").
- `windows-build.ps1` hard-requires **VS2022**; we have VS2019 BuildTools → replicate its steps manually with `-G "Visual Studio 16 2019"`.
- Script path: downloads NVIDIA USD 25.08 py312 (~464 MB) → synthesizes TBB/OpenSubdiv/Imath CMake configs (NVIDIA pkg ships libs but not configs) → cmake configure → build RelWithDebInfo.
- Building.md supports MSVC 19 (VS2019) explicitly; NVIDIA 24.08 section documents the osdGPU/osdCPU link fix (may or may not apply to 25.08).
- pixi.toml is macOS-only (osx-arm64) — pixi path not used for Windows upstream.
