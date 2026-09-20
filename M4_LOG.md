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
