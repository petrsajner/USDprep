## What USDprep is for

CG departments deliver USD scenes built for their own renderers: gigabytes of layered files, hero textures in UDIM tiles, instancing, rigs, renderer-specific materials. A compositor who needs *one prop* or *one character* from such a scene in Nuke usually cannot get it - Nuke's USD reader is strict about what it understands, and nobody on the comp side has the time to learn USD to work around it.

**USDprep closes that gap.** Open the production scene, click the object you need in the 3D view, press Export. What comes out is a small file that Nuke reads - geometry, materials, textures and animation - and a report that says, in plain words, everything that was changed and why. The goal is that a compositor never has to ask a TD for "a version Nuke can open" again.

Every conversion in USDprep is based on **measurement, not assumption**: about 90 small probe scenes were rendered in Nuke 16.1 and 17.0 to find out what Nuke really reads (`NUKE_COMPAT.md`). USDprep offers only what passed.

![USDprep: a character selected in the 3D view, three objects in the export list](https://raw.githubusercontent.com/petrsajner/USDprep/main/docs/images/usdprep_panel.png)

## Built on usdtweak - thank you, Cyril

USDprep is an add-on built on top of [**usdtweak**](https://github.com/cpichard/usdtweak), the open-source USD editor by **Cyril Pichard**. The 3D viewport, the Hydra integration, the add-on mechanism and the whole editor underneath are his work; USDprep adds a panel for compositors, a processing library and a command-line tool on top of it. Without usdtweak this project would not exist in this form - many thanks to Cyril for building it and for keeping it open.

USDprep always starts as the 3D view plus its own panel. usdtweak's editor is still inside and works (Windows menu), but USDprep is a tool for one job; if a USD editor is what you need, install usdtweak itself.

## Highlights

- **Pick by clicking.** Click an object in the 3D view or in the object list; one click takes the whole object. Collect several objects into an export list and write them into one file.
- **Three formats, each verified in Nuke:** USD (`.usdc` + textures folder) for Nuke's current 3D system; **Alembic** (`.abc`, animated) and **OBJ** (`.obj`, still) for the classic 3D system of older Nuke versions - each with a generated `.nk` script that brings the geometry into Nuke with its textures already connected.
- **Small files.** Preview materials instead of hero materials, textures capped at a chosen size, animation trimmed to the shot range or frozen to one frame, optional polygon reduction. ALab's animated outfit: 91 MB -> 7 MB as a still.
- **Every change is a switch, and the original is always one click away.** Nothing is removed silently; the export report lists each conversion by name.
- **Finding your way in heavy scenes:** framing that cuts away the wall between the camera and the object, a "Whole scene" key that shows where the objects are rather than the garden around the set, a searchable object list that follows the 3D view.
- `usdcut` command-line tool with the same features for pipelines and farms; recipes as JSON.
- Self-contained Windows installer: per-user, no admin rights, no internet, nothing else to install. User manual (PDF) included.

### What USDprep fixes in a general USD so that Nuke opens it

Measured in Nuke 16.1v4 and 17.0v1; both behave the same.

| In the scene | What Nuke does with it | What USDprep does |
|---|---|---|
| `.usdz` package | loads the geometry, none of the textures inside | writes `.usdc` + a textures folder |
| Textures in UDIM tiles | black material | stitches the tiles into one atlas, remaps the UVs in the material |
| 8-bit roughness / metallic / normal maps marked `raw` | decodes them as sRGB - too glossy, bent normals | writes re-encoded copies |
| MaterialX next to a standard material | black material | removes the MaterialX output |
| A material only a production renderer understands | the mesh disappears | unbinds it; the mesh shows its display colour |
| Only a `preview` material binding | ignored, grey | binds the chosen material for every purpose |
| Per-face materials (GeomSubsets) | ignored, grey | splits the mesh into one mesh per material |
| Guide and proxy geometry | drawn on top of the real geometry | removed, or hidden if kept |
| Skeletal animation and blend shapes (UsdSkel) | frozen in the rest pose | baked into point caches |
| Z-up scenes | lie on their back | stood up |
| Sphere, Cube, Cylinder, Cone, Capsule | not drawn | turned into meshes |
| Rect and cylinder lights | ignored | replaced by axes of the same name (lights are off by default: any light switches off Nuke's unlit look) |
| Heavily instanced scenes | render stops with "Too many open files" | de-instanced (the `.usdc` does not grow) |
| Textures that are not on this machine | black material, no message | left out and named in the report |
| UVs in a set other than `st`, or outside 0-1 (classic 3D) | wrong or black texture | writes the set the material reads, brings tiles home |
| Curves and volumes | not drawn | cannot be converted - named in the report |

## Install

Download **`USDprep-0.9.0-setup.exe`**, run it, follow its pages. Windows 10/11 64-bit and a graphics driver with OpenGL 4.5 (any machine that runs Nuke 16/17). The manual is in the Start menu after installation and attached below.

## Known limits

Windows only for now. Curves (hair, whiskers) and volumes are reported, not converted. Textures in formats the bundled USD cannot open (`.tx`, TIFF, `.tex`) pass through untouched - Nuke reads `.tx` itself. This is 0.9.0: a release candidate; feedback is welcome in the issues.

---
USDprep is by **Petr Sajner**, Apache License 2.0. Screenshots show ALab by Animal Logic.
