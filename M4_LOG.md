# M4 log — harden & ship

## Slice 1: the self-contained bundle and the Windows installer

`python tools/make-bundle.py --installer` →

- `dist/usdprep-<version>/` — usdtweak with the Prep panel, `usdcut`, every
  DLL they need, USD's plugins, licences. **77 MB, 97 binaries.**
- `dist/usdprep-<version>-setup.exe` — **19 MB**, Inno Setup 6
  (`installer/usdprep.iss`).

How the bundle is put together: nothing is listed by hand. The script
walks the import tables with `dumpbin /dependents`, starting from the two
executables plus every `usd_*.dll` and USD plugin (USD loads those by
name at run time, so no import table mentions them), and copies whatever
resolves inside the conda env. What does not resolve has to be in
`System32`, or the build fails and names it. Layout is the one USD expects
relative to its own DLLs: `bin/`, `bin/usd/`, `plugin/usd/`.

Licences: for every conda package a bundled file came from, the licence
texts are copied from the package cache, and `THIRD_PARTY_NOTICES.txt`
lists name, version and licence (openusd, tbb, opensubdiv, zlib, python,
MSVC/UCRT runtimes) plus usdtweak and meshoptimizer.

The installer: per-user (`PrivilegesRequired=lowest`), default
`%LOCALAPPDATA%\Programs\usdprep`, no internet, Start menu shortcut,
optional desktop shortcut, optional "add usdcut to my PATH" (HKCU only,
removed on uninstall). The first page states the requirements (OpenGL
4.5 driver; "any computer that runs Nuke 16 or 17 qualifies").

### Verified on this machine

- **Clean environment** (`env -i`, PATH = System32 only, bundle copied
  outside the repo): `usdcut extract --preset nuke` on a fixture with a
  UDIM set — reads and writes PNGs, builds the atlas, relinks. So USD's
  plugins (Hio included) are found from the bundle alone.
- **usdtweak from the bundle**, same clean environment: starts, OpenGL
  4.6 (NVIDIA 591.86, RTX 5090), `hdStorm.dll` and `usd_hdSt.dll` loaded,
  and the process has **no module loaded from outside the bundle** other
  than Windows' own. (The window itself was not looked at: screen access
  is granted to the dev build's path only.)
- **Install → run → uninstall**, silent, into a scratch folder: `usdcut
  version` runs from the installed copy; the uninstaller removes the
  folder and the HKCU uninstall entry.

### Decisions

- **Ship the conda-forge binaries** for now, licences included. The
  plan's "source build only for release" was about not shipping NVIDIA's
  prebuilt USD; conda-forge's build is open source all the way down, and
  it is the build every test and every Nuke probe ran against.
- `python314.dll` is in the bundle because conda's USD links against it;
  the Python standard library is not. Nothing in our paths initialises
  Python (both executables ran without it). A USD built without Python
  would drop it — part of the source-build item below.

## Still open in M4

| Item | State |
|---|---|
| Own USD build with OpenImageIO | **not needed for Nuke** - measured: Nuke reads `.tx` itself (`NUKE_COMPAT.md`). OIIO would only let *us* resize/atlas `.tx`/TIFF textures; today those are passed through untouched and reported |
| Linux build + AppImage | not started — no Linux machine here |
| Nuke matrix | 16.1v4 and 17.0v1 measured (`NUKE_COMPAT.md`), identical; 16.0 is not installed |
| First-run GPU check (clear message below OpenGL 4.5) | usdtweak prints the GL version at start; behaviour on an old driver not tested |
| Qualified driver table | one data point: NVIDIA 591.86 / RTX 5090 |
| A licence for USDprep itself | Apache-2.0, the same as usdtweak (`LICENSE`, `NOTICE`) |
| v1.0 tag | after the above |

## Slice 2: the name, the licence, finding your way in the 3D view

- The program and the distribution are **USDprep**: window title,
  `USDprep.exe` in the bundle, installer, Start menu. usdtweak keeps its
  credit in the About box and in `NOTICE`.
- Licence: Apache-2.0 - "the same as usdtweak", which turned out to be
  Apache-2.0, not MIT. Just as free for commercial use and modification.
- **F threw the camera "out of the house".** Framing keeps the viewing
  direction, so for an object in a room a wall ended up between camera
  and object. Two changes in usdtweak, carried as
  `tools/usdtweak-patches/0001-*.patch` and applied by
  `tools/sync-addon.sh`: the fit distance is the bounding sphere in the
  narrower field of view, and while a selection is framed the near
  clipping plane rides just in front of that sphere - what stands in
  front is cut away. Framing the whole scene or walking (right mouse
  button) puts the walls back. Checked on ALab: a tesla coil on a bench
  inside the lab is in full view after F.
- The panel got "Frame selection (F)", "Whole scene (A)" - the way back
  from anywhere - and a "How to move (?)" tooltip with the mouse
  controls. While walking (right button held) the arrows, F and A belong
  to the 3D view, not to the tree.

## Slice 3: older Nuke - .obj and .abc

Petr: `.abc/.fbx/.obj` are the only way to open a 3D object in an older
Nuke, and the test is the classic ReadGeo. Both `.obj` and `.abc` are
written now (`MeshExport.cpp`, `AbcWriter.cpp`), each with a `.nk` that
wires the textures in; details and what the classic 3D taught us are in
`NUKE_COMPAT.md`. The panel offers three formats: USD, Alembic, OBJ.

Alembic, the library: it is **not on conda-forge** (the package called
`alembic` there is SQLAlchemy's migration tool - added by mistake,
removed again). So: `pixi add imath`, and Alembic 1.8.8 built from the
official source archive as a static library, Ogawa only:

```
curl -L -o alembic.tar.gz https://github.com/alembic/alembic/archive/refs/tags/1.8.8.tar.gz
cmake -S alembic-src -B alembic-build -G "Visual Studio 16 2019" -A x64   -DCMAKE_PREFIX_PATH=<env>/Library -DCMAKE_INSTALL_PREFIX=third_party/alembic-install   -DALEMBIC_SHARED_LIBS=OFF -DUSE_TESTS=OFF -DUSE_BINARIES=OFF -DUSE_HDF5=OFF
cmake --build alembic-build --config RelWithDebInfo --target install
```

The core finds it in `third_party/alembic-install` (optional: without it
everything else builds and an `.abc` output fails with a clear message).
The bundle grew by `Imath.dll` - 98 binaries, 80 MB, installer 20 MB;
`.abc` export was run from the bundle in a clean environment.

## Slice 6: usdtweak's dialogs were dead - found and fixed; the author; the version in sight

**No usdtweak dialog opened** (File > Open / Save as, Preferences, Help >
About) - reported by Petr. Bisected with a temporary switch that opened a
test modal by itself and logged ImGui's popup stack: the dialogs died even
with our panel drawing nothing, as soon as *any* window had been focused
with `SetWindowFocus` early on. The log showed why: usdtweak's modal stack
already held one dialog before ours was pushed. usdtweak's **splash screen
is a modal popup** that lives for two seconds; our panel called
`SetWindowFocus` on frame 3 to bring its tab to the front, ImGui closes
popups when another window takes the focus, and the splash's dialog object
stayed on usdtweak's stack for ever - every later dialog queued up behind a
popup that would never be drawn again. (It is also why nobody had ever seen
the splash in USDprep.)

Fixed twice: the panel waits with its focus until no popup is open
(`UsdPrep.cpp`), and usdtweak's `DrawCurrentModal` drops a bottom dialog
whose popup ImGui has closed behind its back (in the usdtweak patch, now
`tools/usdtweak-patches/0001-usdprep-changes-to-usdtweak.patch`). Checked in
the running application: the splash shows, Help > About and File > Open
open with our tab in front.

"Playback runs as soon as the program starts" - the Debug window's
"16.6 ms/frame (60 FPS)" is the interface redrawing itself, which an
immediate-mode GUI does all the time; timeline playback (`_isPlaying`) is
only started by Space in the 3D view or the timeline's play button.

**Author and version.** Petr Sajner is named where usdtweak's author is:
manual (cover, footer, licence chapter, PDF metadata), installer
(publisher, copyright, version info), `usdcut help`, README, NOTICE, the
version tooltip in the panel, and the top of Help > About (a small hook in
the usdtweak patch: `usdtweak::AddAboutLine`). The version is in the window
title ("USDprep 0.9.0"), at the right end of the panel's second row, in
Help > About and on every page of the manual. The manual has a section
"USDprep and usdtweak": built on it, most of the editor starts hidden,
nothing was removed - Windows menu for its panels, Tools menu for ours.

**Always the 3D view and our panel** (Petr: "this is USDprep for Nuke; we
do not hide usdtweak, but we do not offer it either - whoever wants
something else installs plain usdtweak"). Every start closes usdtweak's
panels and the windows of other addons, keeps Viewport1 and opens our
panel - also when it had been closed: the tidy-up runs in an `onStartup`
hook added to usdtweak's addon descriptor by our patch, called once on the
first frame whether the addon's window is open or not. Checked from a
settings file with our panel closed and outliner, property editor and debug
window open: the program came up with the 3D view and the panel. What the
user opens stays for the session.

## Slice 5: v0.9.0 - manual, plan, and a third round of measurements

- Version 0.9.0. `STUDY_AND_PLAN.md` opens with a table of where the plan
  ended up and why. The panel was driven format by format in the running
  application (USD, Alembic, OBJ from the same selection); an `.abc` and an
  `.obj` of the same name no longer share a script: `name_abc.nk`,
  `name_obj.nk`, `name_abc_parts/`.
- **User manual**: `tools/make-manual.py` builds
  `docs/manual/USDprep_User_Manual.pdf` (10 pages, English, screenshots
  taken with `tools/capture-window.ps1`); `make-bundle.py` rebuilds it and
  puts it into the bundle, the installer adds a Start menu shortcut.
  Installation is the last, short chapter - the installer leaves little to do.
- **Shading inputs, colour spaces, blend shapes, volumes** measured in both
  Nukes (`NUKE_COMPAT.md`): roughness, metallic and normal maps are used;
  blend shapes are not evaluated (our bake covers them); volumes are not
  drawn (reported); and every 8-bit texture is decoded as sRGB whatever
  `sourceColorSpace` says - 8-bit data textures marked raw now get a
  re-encoded copy (`RawTextures.cpp`; grey-128 roughness: peak 0.99 before,
  0.06 after, equal to constant 0.5).
- **Corrected: DistantLight works in Nuke** - the first probe used an
  intensity of 3 on a scale whose default is 50000. Found because the ASWF
  normal-map asset, lit by four distant lights, rendered lit. Distant lights
  stay lights.

## Slice 4: de-instancing, measured; "Whole scene" that finds the scene

**De-instancing.** The assumption was "de-instancing is the size trap,
keep instancing by default". Measured on ALab's whole set (1431
instances of 387 prototypes, nuke preset, one frame):

| | file | textures | export |
|---|---|---|---|
| de-instanced | 174 MB | 120 MB | 17.5 s |
| instancing kept, before | 272 MB | **6.1 GB** | 90 s |
| instancing kept, now | 174 MB | 120 MB | 13.6 s |

- Kept instancing was the trap: nothing reached inside the instances -
  a traversal does not visit the prototypes (`over` prims in the
  flattened layer) and an instance proxy cannot be edited. Proxies, hero
  materials with their UDIM sets and lights all stayed.
- Fix (`ExposedPrototypes`, `Shared.h`): for the time of the post-pass
  the `Flattened_Prototype_N` prims are made ordinary defined prims, so
  every pass handles them like any other subtree - once per prototype -
  and they are turned back before saving. The Y-up conversion skips
  them; the .obj/.abc export walks instance proxies.
- The file is no smaller with instancing kept: `.usdc` stores identical
  arrays once, so de-instanced copies cost nothing on disk. What
  instancing could save is Nuke's memory and time - and measured, it
  does the opposite on a big scene:

| ALab set, ScanlineRender2, one frame | Nuke 16.1v4 | Nuke 17.0v1 |
|---|---|---|
| de-instanced (each alone, fresh process) | 10.3 s | 11.7 s |
| instancing kept, 1431 instances | **fails**: `Jpeg read error: Too many open files` | **fails**, same error |
| instancing kept, one bench, 78 instances | 2.6 s, textured | - |

  Nuke opens the textures per instance and runs out of file handles; the
  failing texture is a different one every run. So **de-instancing stays
  the default, for a measured reason**. Kept instancing is still there as
  a switch (small selections are fine), and the report warns above 300
  instances.

**"Whole scene (A)"** framed the bounding box of everything - in ALab
that is the house and its garden, with the lab a detail inside. It now
frames *where the objects are*: the centres of the scene's components,
the middle 80 % on each axis, grown by a typical object's size
(`SceneOverview.h`, worked out once per scene), and cuts into that box
(`FrameCameraOnBox(box, cutaway)` in the usdtweak patch) so a room is
seen from the inside. Checked on ALab: the lab with its benches and the
character, not the garden.

## 0.9.1 - opening files, OBJ import, a hundredth of the polygons (2026-09-24)

Three requests from use in production (Petr, in New Zealand):

**Decimation for scans.** The Geometry choice is now a slider with six
stops - as it is, 1/2, 1/4, 1/10, 1/25, 1/100 (`ExportPanel.cpp`). A
recipe's ratio lands on the nearest stop by the log of the ratio.
Measured on a 2 M-triangle scan with 625 UV charts: 1/100 is reached
exactly, in ~3 s, and renders cleanly in Nuke (NUKE_COMPAT.md, fourth
measurement).

**Opening files crashed the program on the studio's DFS drive**, while
the same folders mapped straight from the server worked. The likely
cause (not reproduced here - see the last point): usdtweak's own file
browser asked the file system about every entry of a folder with
`std::filesystem` calls that throw on any error, sorted with a
comparator that asked the network again at every comparison, and read
each row's date through the network every frame. A DFS link is a reparse
point the client must follow to its target; when that fails, a throw
ends the program, and a sort whose answers change is undefined behaviour.
Changes:

- *File > Open* is the Windows dialog now (`NativeDialogs.cpp`, the
  `onOpenDialog` hook in the usdtweak patch): it reads every drive
  Explorer reads, takes a pasted path, remembers its folder.
- usdtweak's browser (still used by its other dialogs) lists a folder
  once, with error codes: an entry that cannot be asked about is shown
  as what the listing says it is, a folder that cannot be read shows the
  reason, sorting and drawing use the answers in hand, a pasted path may
  come in quotes, and network drives are not asked for their volume name
  at start-up.
- Every file-system call in our code that could throw on a network error
  uses error codes now; the export and the import worker catch whatever
  is left and report it instead of closing the program.
- Not the cause, found on the way: the machine's legacy code page (1252)
  would have made `path::string()` throw on names such as "Předávka" or
  "Tāmaki", but usdtweak switches its C runtime to UTF-8 at start-up, so
  the program itself never did; usdcut would have misread such paths. Both executables and the
  tests now carry a manifest with `activeCodePage` UTF-8 and
  `longPathAware` (`src/windows/usdprep.manifest`).
- Could not be reproduced here: no DFS namespace (and no symbolic-link
  or admin rights to fake one). To be confirmed on the studio drive.

**OBJ import.** An `.obj` is converted to a temporary `.usdc`
(`%TEMP%\USDprep\import`, reused while the source is unchanged, cleared
at the next start) in a background thread with a progress window, then
opened (`SceneOpening.cpp`, the `onOpenStage` / `onFrame` / `onDropFile`
hooks). The converter (`ObjImport.cpp`, also behind `usdcut` for any
command given an .obj) streams the file in 16 MB blocks and parses with
`std::from_chars`: 135 MB in about a second. Objects and groups become
meshes; polygons stay polygons; UVs, normals and vertex colours come
along (per point where they agree, indexed per corner where they do
not); the .mtl becomes UsdPreviewSurface with its maps; per-face
materials become GeomSubsets; texture paths written on another machine
are found next to the .mtl; lines, points and free-form geometry are
reported. Survey coordinates keep their place through a double translate
on the root.

Two older faults surfaced with it, both in 0.9.0 already:

- **An object exported alone lost its material** when the material lived
  elsewhere in the scene (a Looks scope beside the geometry - how every
  imported OBJ is built, and many pipelines). Extract now adds the
  materials the selection is bound to, and the shader nodes their
  networks reach, to the population mask (`MaterialsFromOutside`).
- **Reduced files did not get smaller.** A `.usdc` saved over itself
  appends: a scan decimated to a hundredth still carried its full arrays
  (33 MB instead of 0.5 MB) whenever the relink step did not happen to
  rewrite the file. The post-passes now write the layer anew and swap it
  in (`SaveCompact` / `SwapInPacked`).

Also: the report shows small shares with a decimal ("1.0 %" rather than
"0 %"); an .obj/.abc export warns when world-space floats lose detail far
from the origin; dropping a USD or OBJ file on the window opens it as a
scene; a file that cannot be opened says so.

### Export progress (0.9.1, second round)

Petr, after testing 0.9.1 on the studio's DFS drive (it works): pressing
Export froze the window with the button half-pressed, so an artist could
not tell a working program from a hung one. The export now runs on a
thread of its own (`ExportPanel::Job`); in place of the button a bar
shows how far it is, the step it is on and the time, with a light that
keeps sweeping across it, and a Cancel button. The settings are greyed
out while it runs. Cancel is heard between steps: the run fails with
"cancelled" and removes its temporary folder, so nothing is written (a
step USD cannot interrupt - flattening a big set - finishes first).

The core reports through `usdprep::Progress` (`ExtractOptions::progress`,
atomic fraction / step / cancel) and a thread-local current progress in
`Shared.h` (`Step`, `StepWithin`, `Nudge`, `Cancelled`), so the passes did
not need new parameters. `usdcut extract --progress` prints the steps
with their start times. The weights come from a profile of the whole
ALab set to .usdc (296 s):

| Step | Starts at | Took |
|---|---|---|
| opening, counting, selecting | 0-4 % | 4 s |
| flattening the scene | 8 % | 11 s |
| materials, Nuke conversions | 20-22 % | 0.5 s |
| joining UDIM tiles (per texture) | 25-55 % | 229 s |
| data textures, cards, animation, polygons (per mesh) | 55-80 % | 0.5 s |
| saving | 80 % | 4 s |
| checking / scaling textures (per file) | 82-89 % | 22 s |
| copying the textures | 89 % | 23 s |
| checking the result, done | 94-100 % | 1 s |

The lookup of materials outside the selection now remembers what each
ancestor binds (the selecting step is the masked open, not the lookup).
