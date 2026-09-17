# USD Prep Tool — Study & Plan

**Working name:** *USD Prep* (final name TBD — see Open Questions)
**Date:** 2026-09-17
**Status:** Draft v1.3 for review (v1.1: Nuke 16.0 floor; v1.2: single installer; v1.3: GPU/driver requirements policy)
**Goal:** A small, simple tool that takes production USD scenes from the CG department and turns them into light, Nuke-friendly USD assets — extract one object, delete the rest, optimize, simplify, package.

---

## 0. TL;DR / Recommendation

1. **Base the tool on [usdtweak](https://github.com/cpichard/usdtweak)** — contrary to its stale `master` README, the project is **actively maintained** (commits Aug 2026, monthly installers incl. `win64.exe`), Apache-2.0, C++/ImGui, builds against OpenUSD 25.x, and — crucially — has an official **addons mechanism** designed exactly for "build dedicated tools on top of the main application".
2. **Do not fork-and-diverge.** Stay close to upstream: implement our workflow as an **addon + a shared core library**, so we keep receiving upstream fixes for free.
3. **Ship two faces from one core:** a headless **CLI** (`usdcut`) for batch/farm use, and a **GUI** (usdtweak + our "Prep" panel) for TDs/artists. All logic lives in a UI-free C++ library so both are guaranteed identical.
4. **Deliver one self-contained installer.** Our own branded build of the app (usdtweak with the Prep addon compiled in) plus `usdcut`, with OpenUSD and every dependency bundled. Install → done, 100% functional: **no add-ons to assemble, no admin rights, no internet access**, no Python/DCC prerequisites. Windows first, Linux second. Sole user-side prerequisite: a GPU with vendor drivers (**OpenGL 4.5+**) for the viewport — required up front, never bundled (§5.6).
5. **Target profile: "Nuke-ready USD"** — flattened, self-contained, UsdPreviewSurface/MaterialX materials, Nuke-readable textures, pruned hierarchy, optional decimation, packaged as `.usdz`. One click via a *Nuke preset*.
6. **Pin OpenUSD 25.x** initially (matches Nuke 17's USD 25.08 and usdtweak's own pin `>=25.5.1,<26`); write conservative output readable by **USD 24.05 (Nuke 16.0, our compatibility floor)** and newer. Revisit 26.x later.
7. Rough effort: **~10–13 weeks solo** to a hardened v1 (milestone plan in §6).

---

## 1. Problem & Goals

### 1.1 Situation

- CG will deliver **everything as USD**, typically authored for offline renderers (Arnold/RenderMan/Redshift): heavy composition (dozens of layers/references/payloads), renderer-specific material networks, `.tex/.tx/.rat` textures, UDIMs at hero resolution, full-shot ranges, lights/cameras/props for the whole scene.
- **Nuke is a compositor, not a 3D DCC.** Its USD/Hydra pipeline (new 3D system) can load stages, but unoptimized production scenes make scripts slow to load, memory-hungry and painful to interact with.
- Comp artists need **small, self-contained assets**: "give me just this one prop as a file I can drop into Nuke", or "strip this scene down to what the shot actually sees".

### 1.2 Required operations (from the user's brief)

| # | Operation | Example |
|---|-----------|---------|
| 1 | **Extract** — take one object (subtree) from a scene, save as standalone file | one prop out of a full environment |
| 2 | **Prune / clear** — delete everything except a selection (or delete a selection) | keep hero car, drop the rest |
| 3 | **Cut / slice** — combine 1+2 across multiple inputs | merge two props into one asset |
| 4 | **Optimize** — flatten composition, strip junk, drop unused data | remove renderer materials, unused primvars, dormant variants |
| 5 | **Simplify** — reduce mesh/texture weight | decimate hi-res set geometry, cap texture resolution |
| 6 | **Package** — make the result self-contained and portable | `.usdz`, or folder with relinked textures |

### 1.3 Constraints & priorities

- **Windows primary, Linux secondary.** Completely **English UI**.
- **GPU with vendor drivers is a runtime requirement for the 3D viewport** (any vendor — see §5.6); the app never bundles or installs GPU drivers. Machines without working GPU drivers run in a degraded no-viewport/software-rendering mode (§5.6).
- **Delivery: one self-contained installer.** Install → done: **100% functional with zero add-ons to assemble, no admin rights, no internet access** (at install or run time), no Python/DCC prerequisites. Everything (OpenUSD + plugins, texture/OIIO libs, GUI and CLI) travels inside the package. Windows installer first, Linux package second.
- **Clarity and simplicity are the top priority.** This is *not* a general USD editor — it is a focused preparation utility. Few operations, obvious workflow, good reports.
- Must handle **current** USD (OpenUSD 25.x/26.x) and **Nuke 16.0 and newer** — 16.0 is the compatibility floor, 17.x is the current version.

---

## 2. State of the Ecosystem (September 2026)

### 2.1 OpenUSD today

- Latest stable release: **26.08** (2026-07-20); dev line 26.11. Calendar versioning `YY.MM`; governed with AOUSD (Alliance for OpenUSD) driving spec convergence.
- VFX Reference Platform CY2025 is the current studio baseline; **Nuke 17 ships USD 25.08**.

Relevant built-in capabilities we will build on (no need to reinvent):

| Capability | USD mechanism |
|---|---|
| Load only part of a scene | `UsdStage::OpenMasked` + `UsdStagePopulationMask` (this is what `usdcat --mask /Path` uses) |
| Bake composition into one layer | `usdcat --flatten` / `UsdUtilsFlattenLayer`-family |
| Package assets + textures into one file | `usdzip` / `UsdUtilsCreateNewUsdzPackage` (`.usdz`) |
| Inspect/validate | `usdcat`, `usdchecker`, `usdstitch`, `usdrecord` |
| Reference viewer/editor | `usdview` (ships with USD; view-only + Python console) |

**Implication:** every core operation in §1.2 has a battle-tested USD API underneath it. Our value is **curation, safety and UX** — choosing what to keep, converting what Nuke can't read, reporting what changed — not low-level invention.

### 2.2 Nuke's USD support (what "Nuke-ready" must respect)

Timeline: USD import via `ReadGeo` since Nuke 12.2 (classic 3D) → Hydra-based viewer from 14/15 → **Nuke 16.0 (Feb 2025): new USD-based 3D system arrives (beta)**, ships **USD 24.05**, CY2024 → **Nuke 16.1: Import Scene Graph dialog, MaterialX preview, USD authoring nodes (`GeoEditCamera`/`GeoEditLight`, `GeoPython`)** → **Nuke 17.0 (2025): the new 3D system is fully USD-native**, aligned to **VFX Reference Platform CY2025**, **USD 25.08**, `import pxr` Python available, Nuke ships its own USD build (swappable via `FnUsdShim`).

Version facts that shape our tool (**Nuke 16.0 = our compatibility floor**):

| | Nuke 16.0 (floor) | Nuke 16.1 | Nuke 17.x |
|---|---|---|---|
| USD version | **24.05** | 24.05 (CY2024; not restated in release notes) | **25.08** |
| VFX Reference Platform | CY2024 | CY2024 | CY2025 |
| New 3D system | beta (ScanlineRender2 ray-traced default) | matured + USD authoring nodes | fully USD-native |
| MaterialX preview in Hydra | ❌ | ✅ (`MtlXStandardSurface`) | ✅ |
| Import Scene Graph dialog | ❌ | ✅ | ✅ |
| UsdPreviewSurface in Hydra | ✅ | ✅ | ✅ |

Facts that shape our tool:

- **Import Scene Graph dialog** (16.1+): artists can already load/unload payloads, activate/deactivate prims, and pick elements at import time. **This is complementary, not a substitute:** it doesn't produce a new smaller file, doesn't fix materials/textures, and the choice lives in the Nuke script, not in a reusable asset. Our tool produces the *persistent optimized asset*; Nuke's dialog remains the fallback inside comp.
- **Materials:** Hydra/Storm renders UsdPreviewSurface reliably on all supported versions (16.0+). MaterialX (e.g. `MtlXStandardSurface`) preview works from **16.1** — not on the 16.0 floor, so **UsdPreviewSurface is the default target surface** (see Open Questions). **Renderer-specific networks (Arnold, RenderMan, Redshift) are dead weight in Nuke** — they cost parse time and memory and never render.
- **Geometry:** meshes, point clouds, cameras, lights import; instancing (`UsdPointInstancer`) is handled by Hydra. Deforming/skeletal animation support needs a practical check on real scenes (→ testing matrix, §7; skeletal baking is v2 scope).
- **Texturess:** Nuke/Hydra reads standard formats (png/jpg/tiff/exr); renderer-proprietary texture formats (`.tex`, `.tx`, `.rat`) do **not** load — these must be converted or stripped.

### 2.3 usdtweak — the base (analysis)

Repository: `cpichard/usdtweak`, **Apache-2.0**, default branch `develop`.

**Current state (verified Sep 2026):**

- Active: latest commits **Aug 2026** ("fix patch and build issue on windows", "add glengine facade"); releases roughly monthly (`2026.07.18-prealpha`) with **`win64.exe`, `linux-x86_64.tar.gz`, macOS `.dmg`** installers.
- Stack: **C++ / ImGui / GLFW / OpenGL (Hydra-Storm viewport) / CMake**. Builds against **OpenUSD 25.5+** (conda-forge `openusd >=25.5.1,<26` via pixi on macOS; **NVIDIA prebuilt USD binaries** downloaded by `windows-build.ps1` on Windows — note those binaries are under NVIDIA license terms).
- Feature set today: multi-stage/multi-layer browsing & editing, spec copy/paste, layer-stack management, composition arc authoring (references/payloads/inherits/variants), property & keyframe editing, material assignment, viewport gizmo interaction, layer text editing, experimental connection editor and an experimental natural-language agent ("Twiki").
- **Addons system** (official, documented): build the application with your own C++ tools inside it, *without* managing dynamic libraries. This is explicitly offered as the way to build dedicated studio tools on usdtweak as a foundation.
- Docs: growing GitHub wiki; `doc/Building.md` covers Windows/macOS/Linux builds.

**Assessment against our needs:**

| Our need | usdtweak today |
|---|---|
| Browse/inspect heavy stages | ✅ yes (tree + viewport + property editor) |
| Extract subtree → standalone file | ❌ no one-shot op (manual: author payload/reference surgery, expert-level) |
| Prune/clear scene | ⚠️ possible manually, prim by prim — unusable for "keep these 3 prims of 40 000" |
| Flatten/optimize/package | ❌ not present |
| Nuke-aware conversions (materials, textures) | ❌ not present |
| Batch/CLI | ❌ GUI only |
| Simplicity for non-USD-experts | ❌ it's a general editor; powerful but deep |

**Conclusion:** usdtweak is the right *shell* (it solves the genuinely hard plumbing: USD linkage on Windows, stage browsing, viewport, CI), but the product we need — a guided "load → choose → run recipe → report" workflow — is exactly the kind of thing its addons mechanism was created for.

### 2.4 Alternatives considered

| Tool | Why not |
|---|---|
| `usdview` (ships with USD) | View/inspect only; no editing, no optimization, no packaging UX |
| `usdcat`/`usdzip` CLI | Correct primitives, but raw — no selection UI, no material/texture conversion, no reports; comp TDs won't compose flags by hand |
| [USD Manager](https://github.com/dreamworksanimation/usdmanager) (DreamWorks) | Text/layer editor, not a scene optimizer |
| [3D-Info](https://github.com/cst/Usd3DInfo) (CST) | Lightweight viewer — viewing, not preparing |
| [Gaffer](https://github.com/GafferHQ/gaffer) | Full node-based DCC; far beyond "simple tool" scope |
| NVIDIA Omniverse Composer | Heavy platform, licensing, overkill |
| Houdini / Maya / Blender | DCC-sized; wrong tool to hand to comp; not free/simple for the job |

(Reference list: [awesome-openusd](https://github.com/matiascodesal/awesome-openusd).)

---

## 3. What "Nuke-ready USD" means (target output profile)

The tool's output preset — the definition of *done* for every operation:

1. **Self-contained:** one flattened layer (`.usdc`) or one `.usdz` package; no external layer stack, no absolute paths, textures either relative or packaged.
2. **Composition flattened:** references/payloads/variants/inherits resolved and baked; payloads can no longer stall Nuke's loads.
3. **Hierarchy pruned:** only requested prims (+ their required ancestors/dependencies: bound materials, referenced skel, cameras/lights *if kept*).
4. **Materials Nuke-renderable:** keep/convert to `UsdPreviewSurface` (or MaterialX where the studio adopts it); **strip renderer-specific networks**; keep texture assignments the preview surface needs.
5. **Textures Nuke-readable:** convert `.tex/.tx/.rat` → `exr/png/jpg`; optional resolution cap (e.g. 4K) and UDIM preservation; relink to packaged/relative paths.
6. **Geometry sane:** optional decimation with normals/UVs preserved; unused primvars dropped (keep `st`/UVs, `displayColor`, normals as configured); `purpose` cleaned (drop `guide`/`render`-only junk per policy).
7. **Animation trimmed:** time samples clipped to the requested frame range (or a static frame baked); skeletal animation passed through untouched (bake-to-points = v2).
8. **Metadata clean:** `defaultPrim` set, sensible `upAxis`/units, no dormant variant sets or empty opinions, no renderer-specific metadata blobs.
9. **Report attached:** every run emits a before/after summary (prim/point/triangle/texture counts, file size, what was removed/converted, warnings).

---

## 4. Product Definition

### 4.1 Concept

A **preparation utility**, not an editor. Two faces, one engine:

- **GUI** (TD/artist): usdtweak + our **Prep** workflow — see §4.3.
- **CLI `usdcut`** (pipeline/farm/batch): same operations, same presets, scriptable.

Everything is a **recipe**: an ordered set of named operations with parameters, serializable to JSON, with the **Nuke preset** shipping as default.

### 4.2 Core operations (v1 scope)

| Op | What it does | USD mechanics |
|---|---|---|
| **Inspect** | Stage report: hierarchy stats, prim types, counts, materials, textures (formats/sizes), animation ranges, composition depth, time samples | stage traverse + `UsdUtils` |
| **Select** | Build a keep/drop selection by tree, search/filter (type, name, purpose), or "what uses material X" | traversal rules |
| **Extract** | Copy chosen subtrees into a new standalone stage | `OpenMasked` + population mask + flatten |
| **Prune** | Delete selection (or everything *except* selection), drop by purpose/type (lights, cameras, guides) | stage edit + flatten |
| **Strip** | Remove renderer material networks, unused primvars, dormant variants, junk metadata, unused time samples | layer-level spec edits |
| **Optimize** | Flatten composition; deduplicate identical meshes → instancing; merge compatible meshes; drop invisible/inactive prims | flatten + hashing/compare |
| **Simplify** | Decimate meshes (target ratio or screen-error), preserve UVs/normals | [meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT) simplifier |
| **Textures** | Convert formats, cap resolution, keep UDIMs, relink paths, copy alongside output | OpenImageIO (`oiiotool` semantics, C++ API) |
| **Trim** | Clip time-sample range to `[start, end]` or bake single frame | time-sample editing |
| **Package** | Emit flattened `.usdc` or `.usdz` with textures | `UsdUtilsCreateNewUsdzPackage` |

Explicitly **out of scope v1**: authoring/animating geometry, skeletal animation baking, rendering, USD text syntax editing (usdtweak already provides that next door if ever needed).

### 4.3 GUI workflow (three steps, no more)

```
┌──────────────────────────────────────────────────────────────┐
│ 1. LOAD          open .usd / .usda / .usdc / .usdz           │
│                  → stage stats banner (prims, size, fps?)    │
│ 2. CHOOSE        tree with keep/prune checkboxes + filters   │
│                  (name, type, purpose); viewport pick as     │
│                  assist; "invert", "only selection's deps"   │
│ 3. RUN           recipe panel: preset "Nuke" ▾ + toggles     │
│                  (decimate %, texture cap, keep cameras…)    │
│                  → progress → output file + BEFORE/AFTER     │
│                    report (counts, size, actions, warnings)  │
└──────────────────────────────────────────────────────────────┘
```

CLI mirror:

```
usdcut extract scene.usd /World/Set/Car --preset nuke -o car.usdz
usdcut prune   scene.usd --except /World/Set/Car,/World/Cameras/shotCam --preset nuke -o shot_min.usdc
usdcut inspect scene.usd --report out.json
```

### 4.4 Non-goals / principles

- Never modify the input file(s); always write a new output.
- Deterministic: same input + same recipe ⇒ identical output (golden-file testable).
- No silent decisions: anything removed/converted appears in the report.
- English-only UI, minimal jargon ("Delete renderer-only materials", not "Strip non-previewable Material networks").

---

## 5. Architecture

### 5.1 Layered design

```
┌────────────────────────────┐   ┌──────────────────────────┐
│ GUI: usdtweak + "Prep"     │   │ CLI: usdcut              │
│ addon (ImGui panels,       │   │ (argparse-style, JSON    │
│ tree, viewport pick)       │   │ recipes, exit codes)     │
└─────────────┬──────────────┘   └────────────┬─────────────┘
              │          uses                   │
              └──────────────┬─────────────────┘
                             ▼
              ┌──────────────────────────────────┐
              │ usdprep-core  (pure C++ library) │
              │  StageInfo / Selection /         │
              │  Ops (Extract, Prune, Strip, …)  │
              │  Recipe engine + presets (JSON)  │
              │  Report model (JSON + text)      │
              ├──────────────────────────────────┤
              │ OpenUSD 25.x │ meshoptimizer │   │
              │ OpenImageIO (textures) │ json    │
              └──────────────────────────────────┘
```

- `usdprep-core` has **zero UI dependencies** — compiles headless on Windows/Linux CI from day one.
- The GUI is an **usdtweak addon** binding core results into panels; nothing business-y lives in the GUI layer.

### 5.2 Relationship to usdtweak (fork strategy)

- **Stay upstream-compatible.** We track `develop`, contribute generic fixes upstream (PRs), and keep our product entirely in the addon + core library. No vendored forks of USD.
- The addon is a **build-time concept only**: we compile usdtweak with our addon baked in and ship the result as **our own application** (own name/icon). End users install one app — they never install or even see an add-on.
- Rebranding happens in our build (app name/icon), not in divergent code.
- If upstream stalls, the addon boundary means we can lift the shell and continue independently — the core survives either way.

### 5.3 USD version & compatibility policy

- **Build pin: OpenUSD 25.x** (align with usdtweak `>=25.5.1,<26` and Nuke 17's 25.08).
- **Write conservatively:** output uses only long-established schemas (geometry, UsdPreviewSurface, basic animation), readable by **USD 24.05 (Nuke 16.0, our floor)** and newer; flatten output avoids version-sensitive features.
- Upgrade to 26.x in a dedicated milestone after Nuke validation (§7), keeping 25.x output parity via golden tests.

### 5.4 Dependency & build strategy on Windows (primary platform)

| Option | Pros | Cons | Use for |
|---|---|---|---|
| **pixi + conda-forge `openusd`** (win/linux/osx) | Reproducible, no local USD build, matches usdtweak's own tooling | Conda runtime packaging for shipping app | **Developer workstations (recommended)** |
| NVIDIA prebuilt USD binaries | One-command via usdtweak's `windows-build.ps1` | NVIDIA license terms on the binaries; version choice limited | Quick start / spike |
| vcpkg `usd` port | MSVC-native, manifest-mode | Port lag vs upstream releases | Alternative if conda is unwelcome |
| Build OpenUSD from source (BUILDING.md) | Full control, exact pin | ~hours of build, must maintain scripts | **Release builds of the shipped app** |

Other dependencies: `meshoptimizer` (MIT, header-friendly), OpenImageIO for texture conversion, `nlohmann/json` for recipes/reports. CMake presets for Windows (VS2022) and Linux; CI on GitHub Actions (windows-latest + ubuntu) building core, CLI and addon.

### 5.5 Distribution

**Hard requirement: one self-contained, offline, no-admin installer — "install, done".**

- **Windows (primary):** a single **NSIS per-user installer** — installs under `%LOCALAPPDATA%\Programs\<App>`, needs **no admin rights**, **no internet**, and bundles everything: our own-built OpenUSD (+ plugins/Hydra delegates), OpenImageIO, meshoptimizer, and both executables (GUI + `usdcut`, optional per-user PATH entry). Includes an uninstaller; no Python, no DCC, no separate runtimes. usdtweak already ships bundled `win64.exe` installers, so the packaging pattern is proven.
- **Linux (secondary):** **AppImage** — one file, no installation, no root, bundles the same payload and runs on mainstream distros; `.deb`/`.rpm` only if the studio asks.
- **What never ships:** conda/pixi environments, NVIDIA prebuilt USD binaries (license terms), **GPU drivers (never — vendor/user responsibility, see §5.6)**, anything requiring a download at install time. Dev machines may use those; distributed packages contain only **our own-built, version-pinned dependencies**.
- The installer's welcome page states the runtime requirements up front (GPU with OpenGL 4.5 + installed vendor driver, link to the qualified-driver table) — no surprises after install.
- The CLI stays a separate process — it must never be loaded into Nuke's Python (Nuke's internal USD version must not be polluted by ours).

### 5.6 Runtime requirements & GPU/driver policy

**What the viewport actually needs.** The 3D viewport is rendered by Hydra **Storm** through its OpenGL backend (**HgiGL**), which requires **OpenGL 4.5** from the GPU driver. This is standard, cross-vendor OpenGL — **not** an NVIDIA-only technology: no CUDA, no OptiX, no RTX, no vendor SDK. Any NVIDIA / AMD / Intel GPU with a reasonably current driver provides it (roughly: GPUs from ~2015 onward — NVIDIA Maxwell+, AMD GCN 1.2+, Intel Broadwell/Arc+).

**Why we never ship drivers.** GPU drivers are kernel-level, vendor-owned, and must match the exact hardware — no application installs them (Nuke, Photoshop, games all require the user's own driver). What we avoid shipping is NVIDIA's *prebuilt USD binaries* (license issue) — that is unrelated to drivers.

**Reality check for our audience:** Nuke itself already requires a GPU with current drivers (NVIDIA compute capability 3.5+, CUDA 12.8-class driver ≈ **571.96+ on Windows**; Foundry qualifies exact driver versions per release). Our requirement (OpenGL 4.5) is *lighter* than Nuke's — any machine that runs Nuke 16/17 can run our tool's viewport.

**Degraded modes (machine without usable GPU/driver, e.g. RDP session or broken driver):**

1. **First-run check:** app queries OpenGL version at startup; below 4.5 → clear dialog naming the problem and the fix ("update your GPU driver"), never a crash or a black viewport.
2. **No-viewport mode:** our core workflow (Load → tree selection → Run) does not need rendering; the Prep panel works with the viewport hidden. `usdcut` CLI is fully headless and never touches the GPU.
3. **Optional bundled Mesa (llvmpipe) software-OpenGL fallback** for driver-less machines: functional but slow — acceptable for small scenes, explicitly warned in-UI; decide during M0 testing whether to ship it.

**Published minimum spec (installer welcome page, release notes, docs):**

| Item | Minimum | Recommended |
|---|---|---|
| OS | Windows 10/11 64-bit (Linux x86-64 for AppImage) | — |
| GPU | any GPU + vendor driver with **OpenGL 4.5** | modern GPU, ≥2 GB VRAM |
| GPU driver | vendor-qualified version — table per vendor below, filled by measured qualification in M0/M4 | latest stable from vendor |
| RAM | 8 GB | 32 GB (heavy scenes) |

**Qualified driver versions** (Foundry-style: we test against specific versions and publish them — placeholders until measured):

| Vendor | Minimum qualified driver (Windows) | Notes |
|---|---|---|
| NVIDIA | *TBD in M0 — expected ~5xx branch or newer* | any Maxwell+ GPU |
| AMD | *TBD in M0* | Adrenalin branch |
| Intel | *TBD in M0* | Arc/Iris driver |

**Millions of polygons:** Storm on a modest current GPU handles multi-million-triangle scenes interactively; our pipeline's own prune/decimate/flatten steps reduce what reaches the viewport in the first place. Performance envelope to be measured and documented on benchmark scenes (§7).

---

## 6. Roadmap (solo-developer estimates)

| Milestone | Contents | Exit criteria | Est. |
|---|---|---|---|
| **M0 — Spike** | Build usdtweak on Windows (pixi + NVIDIA-script paths); run an addon sample; hand-test `usdcat --flatten --mask` + `usdzip` on a real CG scene; confirm Nuke 16.0 + 17 load the flattened result; **verify viewport/GL behavior on the target machine + decide Mesa fallback yes/no** | One real scene → one extracted prop → loads in Nuke, before/after numbers captured | 1 wk |
| **M1 — Core + CLI MVP** | `usdprep-core`: Inspect, Select, Extract, Prune, Flatten, Package + Nuke preset v0 + JSON reports; `usdcut` CLI; unit + golden tests | The three CLI examples from §4.3 pass on 3 test scenes | 2–3 wk |
| **M2 — GUI addon** | Prep panel in usdtweak: load → choose (tree/filters/viewport assist) → run → report view | A non-USD-expert TD extracts a prop without docs | 2–3 wk |
| **M3 — Optimize & simplify** | Strip (materials/primvars/variants/metadata), Textures (convert/cap/relink), Simplify (meshoptimizer), Trim (frame range); preset v1 | ≥70% size/load-time reduction on benchmark scene (target, validated in Nuke) | 3–4 wk |
| **M4 — Harden & ship** | Linux build, CI matrix, **single self-contained offline installer (Win, per-user) + AppImage (Linux)**, docs (short!), test matrix vs Nuke 16.0/16.1/17.x, error handling pass (incl. first-run GPU/driver check), **qualified GPU-driver table measured & published**, v1.0 tag | Installer + AppImage, both installable/runnable **without admin or internet** on clean machines; known-issues list published | 2 wk |

Total: **~10–13 weeks** to v1.0. M1 already delivers daily value via CLI even before any GUI exists.

---

## 7. Validation & Testing

- **Corpus:** Pixar sample sets (e.g. Kitchen_set, City_set), USD files from Asset Validator suite, plus 3–5 real CG deliveries (under NDA, stored locally).
- **Nuke matrix:** Nuke 16.0 (**compatibility floor**), 16.1, 17.x (primary) — measure: script load time, first-frame draw time, viewport interaction FPS, memory footprint; compare raw vs. prepared.
- **Golden-file tests:** recipe + input ⇒ byte-stable `.usdc` (and stable report JSON) on CI.
- **Compatibility guard:** every release batch-checks outputs with `usdchecker` + roundtrip open in pinned OpenUSD **24.05** (Nuke 16.0 floor) and 25.08 containers.
- Edge cases: deeply nested payloads, instanceable prims, PointInstancer scattering, UDIM sets, skeletal-animated props (pass-through check), `.usdz` inputs, unicode/UNC paths on Windows.

---

## 8. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| usdtweak is pre-alpha & moves fast | Addon breakage | Pin releases we track; core library keeps zero usdtweak deps; CI rebuilds weekly |
| Self-contained bundle = large installer & packaging effort | Slower M4, disk footprint | usdtweak's own bundled installers prove the pattern — reuse its packaging; size is a one-time cost, documented in release notes |
| OpenUSD build/packaging pain on Windows | Slowed M0/M4 | pixi/conda-forge for dev; proven `windows-build.ps1`; source build only for release |
| NVIDIA prebuilt USD license terms | Legal if shipped | Ship only our own-built USD (conda/source) in distributed packages |
| Nuke's Hydra quirks (textures, MaterialX) | Output not as light as hoped | M0 proves on real scenes first; env workarounds documented (e.g. `USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX`) |
| Target machine without working GPU drivers / RDP sessions | Viewport unusable ("HgiGL minimum OpenGL requirements not met") | First-run GL check with actionable message; no-viewport mode (core workflow renders nothing); optional Mesa llvmpipe fallback (M0 decision); `usdcut` always works headless |
| Viewport performance on multi-million-poly scenes | Users distrust the tool | Storm handles big scenes on modest GPUs; our recipes prune/decimate before viewing; benchmark + publish measured envelope (§7) |
| Mesh decimation damaging hero assets | Artist distrust | Opt-in only, ratio preview + report, original never touched |
| Skeletal animation not Nuke-friendly | Some assets stay heavy | Pass-through in v1; bake-to-points scheduled as v2 spike |
| Scope creep toward "another USD editor" | Simplicity death | §4 non-goals enforced; every feature must fit the 3-step UX or be rejected |

---

## 9. Open Questions

1. **Name** (working name "USD Prep"). Candidates to vet: *UsdCut*, *Pare*, *Sift* — needs a naming pass (check trademarks/collisions) before v1 branding.
2. Texture policy defaults: hard cap (4K?) allowed by the studio? EXR vs PNG/JPG for diffuse? UDIM handling on conversion.
3. Keep cameras/lights by default in the Nuke preset, or drop by default? (Comp often wants the shot camera.)
4. MaterialX adoption: with the Nuke 16.0 floor, **UsdPreviewSurface is effectively decided as the default target surface** (MaterialX preview only exists from 16.1). Remaining choice: also emit an optional duplicate MtlX network for 16.1+/17 users, or skip MtlX entirely in v1?
5. Distribution: internal-only, or shared publicly (affects licensing review of every dependency; all currently permissive).
6. v2 candidates & priority: skeletal bake-to-points, proxy/bbox purpose generation, watch-folder automation service.
7. GPU driver qualification: fill the per-vendor minimum driver table (§5.6) with measured versions from M0/M4 test machines; decide whether to bundle the Mesa software-GL fallback for driver-less machines.

---

## 10. Sources

- usdtweak: [repo](https://github.com/cpichard/usdtweak) · [wiki](https://github.com/cpichard/usdtweak/wiki) · releases `2026.07.18-prealpha` (win64/linux/mac installers) · Apache-2.0 · `develop` branch, commits Aug 2026
- OpenUSD: [releases](https://github.com/PixarAnimationStudios/OpenUSD/releases) (26.08, 2026-07-20) · [BUILDING.md](https://github.com/PixarAnimationStudios/OpenUSD/blob/dev/BUILDING.md) · [toolset](https://openusd.org/dev/toolset.html) · [products using USD](https://openusd.org/release/usd_products.html)
- Nuke: [What's New 16.0](https://learn.foundry.com/nuke/content/release_notes/nuke_16.0.html) · [17.0 release notes](https://learn.foundry.com/nuke/17.1v1/content/release_notes/nuke_17.0.html) (USD 25.08, CY2025, new 3D system, Import Scene Graph) · [USD in classic 3D](https://learn.foundry.com/nuke/content/comp_environment/3d_compositing/usd.html) · [USD concepts, new 3D system](https://learn.foundry.com/nuke/content/comp_environment/usd-3d-comp/usd-concepts.html)
- Packaging/build: [conda-forge openusd](https://anaconda.org/conda-forge/openusd) (win/linux/osx) · [vcpkg usd port](https://vcpkg.link/ports/usd) · [AOUSD forum on C++ USD apps](https://forum.aousd.org/t/building-a-c-application-with-usd-libraries/1899) · NVIDIA prebuilt USD binaries (via usdtweak build script; NVIDIA license terms)
- GPU/viewport: [HdStorm docs](https://openusd.org/dev/api/hd_storm_page_front.html) · ["HgiGL minimum OpenGL requirements not met" — OpenUSD #2756](https://github.com/PixarAnimationStudios/OpenUSD/issues/2756) (OpenGL 4.5 requirement, RDP/llvmpipe pitfalls) · [Khronos: Vulkan backend for Hydra Storm](https://www.khronos.org/blog/vulkan-support-added-to-openusd-and-pixars-hydra-storm-renderer) · [Foundry system requirements (Nuke GPU/CUDA qualification)](https://www.foundry.com/products/nuke-family/requirements)
- Alternatives: [awesome-openusd](https://github.com/matiascodesal/awesome-openusd) · [usdmanager](https://github.com/dreamworksanimation/usdmanager) · [Gaffer](https://github.com/GafferHQ/gaffer) · [meshoptimizer](https://github.com/zeux/meshoptimizer)
