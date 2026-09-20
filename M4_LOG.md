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
| Own USD build with **OpenImageIO** (`.tx/.tex` textures) and **Alembic** (level-3 `.abc`) | not started — hours of compiling and ~1 GB of source downloads; both need a Nuke probe first ("only what Nuke reads") |
| Linux build + AppImage | not started — no Linux machine here |
| Nuke matrix | 16.1v4 and 17.0v1 measured (`NUKE_COMPAT.md`), identical; 16.0 is not installed |
| First-run GPU check (clear message below OpenGL 4.5) | usdtweak prints the GL version at start; behaviour on an old driver not tested |
| Qualified driver table | one data point: NVIDIA 591.86 / RTX 5090 |
| A licence for usdprep itself | none in the repo yet — Petr's call |
| v1.0 tag | after the above |
