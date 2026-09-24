## What 0.9.1 is about

0.9.1 comes straight from using 0.9.0 in production. Three things got in the way: a studio project drive that USDprep could not open, lidar and photogrammetry scans far too heavy for a comp, and an export that looked like a frozen program. All three are solved, and two older faults that surfaced on the way are fixed too.

![USDprep 0.9.1: a converted OBJ scan, reduced to a hundredth of its polygons, the export running](https://raw.githubusercontent.com/petrsajner/USDprep/main/docs/images/usdprep_091.png)

## New and changed

### Opening files - network drives and DFS work

- **File > Open is the Windows dialog now.** It reads every drive Windows Explorer reads - mapped network drives, DFS namespaces, OneDrive - takes a pasted path and remembers its folder. Tested on a studio DFS drive.
- **0.9.0 crashed on a DFS drive**, in the file browser of usdtweak, the editor USDprep is built on. The likely reason: it asked the network about every entry of a folder in every frame, and a single error ended the program. That browser (still used by the editor's other dialogs) now lists a folder once, shows a folder it cannot read instead of crashing, and accepts a pasted path in quotes, as Explorer's *Copy as path* gives it.
- **Drop a file on the window** to open it.
- **A file that cannot be opened says why**, instead of nothing happening.

### Scans and OBJ files

- **OBJ files open directly.** They are converted to a temporary USD file in the background, with a progress window; the original is only read. From then on it is a scene like any other: pick, reduce, export.
- What comes along: every object and group, the polygons as they are, UVs, normals and **vertex colours**, and the `.mtl` materials with their textures (colour, opacity, roughness, metallic, emission, normal maps). A texture path written on another computer is looked for next to the `.mtl`. The panel header says *converted*; hover over it for what the conversion found and what it had to leave out.
- **Survey coordinates keep their place.** A scan millions of units from the origin is stored around its centre, with the offset on its root in double precision - the exported file puts it exactly where it was, and Nuke renders it without float jitter. The 3D view, which draws in single precision, shows such a scan at the origin and says so.
- `usdcut` takes an `.obj` for every command: `usdcut extract scan.obj /scan -o scan_small.usdc --preset nuke --simplify 0.01`

### A hundredth of the polygons

- **The Geometry setting is a slider with six stops:** as it is, 1/2, 1/4, 1/10, **1/25** and **1/100** of the polygons. The last two are for scans: a light stand-in for the comp.
- Measured on a two-million-triangle scan with its UVs cut into 625 charts: 1/100 is reached exactly, in about three seconds, and the texture stays clean across the UV seams in Nuke 16.1 and 17.0 (identical pixels in both).

### The export shows what it is doing

- **The export runs in the background.** In place of the button, a bar shows how far it is and the step it is on (*flattening the scene*, *joining UDIM tiles*, *reducing polygons*...), with a light that keeps sweeping across it and the elapsed time - a long export never looks like a hung program. The window stays usable.
- **Cancel** stops the export and nothing is written. A step USD cannot interrupt (reading a big set) finishes first.
- `usdcut extract --progress` prints the steps with their times, to see where a long run spends it.

![The panel during an export: the converted scan, the Geometry slider at 1/100, the progress bar](https://raw.githubusercontent.com/petrsajner/USDprep/main/docs/images/usdprep_091_panel.png)

## Fixed

Both of these were already in 0.9.0:

- **An object exported on its own lost its material** when the material lived elsewhere in the scene - a *Looks* or *Materials* scope beside the geometry, as in many pipelines and in every converted OBJ. The materials an object uses now come along, with the shader nodes they need.
- **Reduced files did not get smaller.** A scan reduced to a hundredth could still carry its full arrays - 33 MB instead of 0.5 MB - because a `.usdc` saved over itself keeps what it held. The file is written anew now, so every reduction shows in its size.

Also:

- `usdcut` reads and writes paths with any characters (UTF-8 throughout) and long paths where Windows allows them.
- An `.obj` or `.abc` export warns when the geometry sits so far from the origin that world-space floats lose detail - Nuke's classic 3D would show it coarse. The USD export keeps the detail.
- The report shows small shares precisely: "1.0 %" instead of "0 %".

## Measured in Nuke

Imported OBJ scans, exported full and at 1/10, 1/25 and 1/100, at the origin and at survey coordinates, plus an OBJ with several objects and per-face materials: all rendered in **Nuke 16.1v4 and 17.0v1** with textures, pixel-identical between the two. Details in [NUKE_COMPAT.md](https://github.com/petrsajner/USDprep/blob/main/NUKE_COMPAT.md) (fourth measurement).

## Install

Download **`USDprep-0.9.1-setup.exe`**, run it, follow its pages - it installs over 0.9.0. Windows 10/11 64-bit, per user, no admin rights, no internet, nothing else to install. The updated user manual (new section *Scans and OBJ files*) is in the Start menu after installation and attached below.

## Known limits

Windows only for now. OBJ lines and points are not imported (USDprep reads polygon meshes); PLY and E57 are not read yet. Curves (hair, wires) and volumes are reported, not converted. Cancel waits for a step USD cannot interrupt. This is still an early version - feedback is welcome in the issues.

---
USDprep is by **Petr Sajner**, Apache License 2.0, built on [usdtweak](https://github.com/cpichard/usdtweak) by Cyril Pichard. The scan in the screenshots is a synthetic test scene.
