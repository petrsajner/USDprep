# What Nuke reads — measured

67 tiny USD scenes, one question each, rendered through `GeoImport` →
`ScanlineRender2` and measured (coverage, colour, position) — no opinions,
no documentation. Generator `tools/nuke/make_probes.py`, runner
`tools/nuke/run_probes.py`, raw numbers in
`tools/nuke/results/` (`results_nuke16.json`, `results_nuke17.json`, `manifest.json`).

| | Nuke 16.1v4 | Nuke 17.0v1 |
|---|---|---|
| bundled USD | 24.05 | 25.08 |

**The two versions gave the same answer on every single probe.** One
column is enough.

## Files and textures

| Question | Nuke | |
|---|---|---|
| `.usda`, `.usdc` with a texture next to them | ✅ | |
| `.usdz` with the texture inside | ❌ geometry yes, texture black | `.usdz` stays off the menu |
| PNG, JPEG, EXR, TIFF textures | ✅ | |
| `<UDIM>` template, several tiles | ❌ black | → UDIM atlas |
| `<UDIM>` template, a single tile | ❌ black | → rewritten to the tile |
| `UsdTransform2d` | ✅ | what the atlas relies on — fine in 16 too |
| a texture that is not there | black material, no error | we leave them out and say so |
| unflattened references, variant selections | ✅ | flattening is for portability, not for Nuke |

## Materials

| Question | Nuke | |
|---|---|---|
| UsdPreviewSurface: constant colour, texture, emissive | ✅ | |
| opacity, constant and from a texture | ✅ lands in alpha | |
| no material, `displayColor` | ✅ | |
| no material, no colour | grey 0.18 | |
| binding inherited from a parent | ✅ | |
| `material:binding:full` next to `:preview` | shows **full** | |
| only `material:binding:full` | ✅ honoured | |
| only `material:binding:preview` | ❌ ignored, grey | our "bind for every purpose" is what makes the light material show |
| per-face materials through GeomSubsets | ❌ ignored, grey | **gap** |
| MaterialX only (`outputs:mtlx:surface`) | ❌ black | |
| MaterialX **next to** UsdPreviewSurface | ❌ **black** — the mtlx output wins and fails | stripping renderer outputs is not optional for Nuke |
| arnold output next to UsdPreviewSurface | ✅ preview shown | |
| material with only a renderer output, or no output at all | ❌ **the mesh disappears** | **gap** |

## Geometry

| Question | Nuke | |
|---|---|---|
| quads, triangles, n-gons | ✅ | |
| subdivision (`catmullClark`) | not applied, the cage is drawn | Simplify turning it off loses nothing |
| `leftHanded`, single-sided faces seen from behind | drawn, nothing is culled | |
| `visibility = invisible`, `active = false` | ✅ respected | |
| purpose `render`, `proxy`, `guide` | ❌ **all three are drawn** | an asset with proxies shows twice — dropping guide/proxy is not optional for Nuke |
| implicit shapes: Sphere, Cube, Cylinder, Cone, Capsule | ❌ not drawn | **gap** |
| BasisCurves | ❌ not drawn | (hair, wires) |
| Points | ✅ | |
| native instancing (`instanceable`) | ✅ small scenes (78 instances: fine) · ❌ big ones: at 1431 instances the render stops with `Too many open files` | de-instance for Nuke; the `.usdc` does not grow |
| PointInstancer | ✅ | |
| `upAxis = "Z"` | ❌ not converted, the scene lies on its back | **gap** |
| `metersPerUnit` | ignored, units are taken as they are | |

## Animation

| Question | Nuke | |
|---|---|---|
| animated transforms | ✅ | |
| animated points (point caches) | ✅ | |
| animated visibility | ✅ | |
| production frame numbers (1001…) | ✅ | |
| UsdSkel skinning | ❌ **not evaluated** — the mesh stays in its bind pose | **gap** |

## Lights and cameras

| Question | Nuke | |
|---|---|---|
| no light anywhere | surfaces show their plain colour (unlit) | the friendly default |
| DistantLight, SphereLight, DiskLight, DomeLight in the file | used — and the unlit default is gone | a dome at production intensity blows out |
| DistantLight in the file | used, direction and falloff right - on UsdLux's own intensity scale (default 50000). **Corrected:** the first probe used intensity 3, which is next to nothing on that scale, and was misread as "gives nothing" | the ASWF normal-map test, lit by four distant lights, showed it |
| RectLight, CylinderLight | ignored altogether | |
| Camera in the file | ✅ through the Camera node's import (translate, focal) | |

## Textures in `.tx`, and the level-3 formats (second measurement)

Same result in Nuke 16.1v4 and 17.0v1 (`tools/nuke/make_tx.py`,
`tools/nuke/probe_formats.py`):

| Question | Nuke | |
|---|---|---|
| `.tx` texture (tiled, mip-mapped TIFF - the structure maketx writes; hand-built, 8-bit, uncompressed) | ✅ read, same colour as the PNG | a `.tx` can go to Nuke as it is |
| the same file named `.tif` | ✅ | it is the TIFF reader doing it |
| `.abc` written by Nuke, read through GeoImport (the USD 3D system) | ✅ | |
| `.obj`, `.fbx` through GeoImport | ❌ nothing is drawn, no error | |
| `.obj`, `.fbx` through the classic ReadGeo | inconclusive - the classic render rig of the probe did not produce an image | |

Not measured: compressed / half-float / EXR-based `.tx` as a renderer's
maketx would write them (no working maketx on this machine), and
RenderMan's `.tex`, which is a different format altogether.

**Correction after a second look:** the classic rig of that probe was
wired wrong. Through the classic `ReadGeo` + `ScanlineRender` - the only
3D an older Nuke has - **`.abc`, `.obj` and `.fbx` are all read, UVs
included** (a checkerboard fed into ReadGeo shows on the sphere), in 16.1
and in 17.0 alike. Two traps for scripts: a ReadGeo's `all_objects` knob
belongs to `.fbx` only (set on an `.abc` it empties the node), and
`scene_view` is best left alone.

What follows for the plan:

- **OpenImageIO is not needed to get `.tx` into Nuke** - Nuke reads it.
  It would only be needed for *us* to open such textures (the resolution
  cap and the UDIM atlas skip what Hio cannot read, and say so), and for
  `.tex`.
- **`.obj` / `.abc` / `.fbx` are the way into an older Nuke** (classic 3D
  only), which does not read USD the way 14+ does. They carry geometry
  and UVs; Nuke reads no materials from any of them, so the textures come
  through a generated `.nk`.
  - `.obj` - **done** (`src/core/src/MeshExport.cpp`): a still in world
    space, one file per material next to the complete one, and a `.nk`
    with ReadGeo nodes, textures wired in, joined by a Scene. Loaded with
    `nuke.scriptReadFile` and rendered through the classic ScanlineRender
    in 16.1 and 17.0 (`tools/nuke/probe_nk.py`): ALab's projector comes up
    textured, per-material colours arrive, UDIM-atlas UVs are baked into
    the file.
  - `.abc` - **done** (`AbcWriter.cpp`): the same export with the
    animation inside - world-space positions sampled at every frame of
    the (trimmed) range, topology and UVs once; one frame asked for gives
    a still. Checked in the classic 3D of 16.1 and 17.0: a skinned quad
    (baked by the compat pass) travels left to right over frames 1-10,
    and ALab's outfit - two meshes, 54 frames of cloth simulation,
    26 MB, 1.5 s to export - comes up textured and moving.
  - `.fbx` - not written: it needs Autodesk's FBX SDK (proprietary) or a
    hand-written ASCII writer, and adds nothing over `.obj` + `.abc`.

Three things the classic 3D taught us, all handled in `MeshExport.cpp`:

- **A ReadGeo made by a script loads only the first object of an
  `.abc`.** Python reports every item as imported, the render shows
  one. With the items listed in the `scene_view` knob of the `.nk`
  (`{{0} imported: 0 1 selected: 0 1 items: /root/a/aShape ...}`) all of
  them load - so the `.nk` lists them.
- **The UVs are whatever the material reads, not what is called `st`.**
  ALab's preview materials look their texture up with `perfuv`; taking
  `st` gave a sweater with black patches. The exporter follows the
  texture's `st` input to its primvar reader (through our UDIM-atlas
  transform if there is one) and writes that set.
- **Classic Nuke shows black outside 0..1** where a renderer repeats the
  texture. UVs that sit in another tile under a single repeating texture
  are moved home face by face (each corner has its own UV in these
  formats, so a face can move by whole tiles).

## Shading inputs, colour spaces, blend shapes, volumes (third measurement)

Probes `tools/nuke/make_probes_shading.py` (a sphere light at the camera,
so roughness and normals show), plus the ASWF USD working group's test
assets rendered before and after USDprep (`tools/nuke/make_material_tests.py`,
`third_party/usd-wg-assets`, Apache-2.0). Same in 16.1v4 and 17.0v1.

| Question | Nuke | |
|---|---|---|
| `roughness`, constant and from a texture | used: the highlight peak goes 59.7 / 0.06 / 0.04 for roughness 0.1 / 0.5 / 0.9 | |
| `metallic` | used | |
| normal map (`inputs:normal` from a texture, scale 2 / bias -1) | used | |
| `sourceColorSpace` on an 8-bit texture | **ignored: every 8-bit texture is decoded as sRGB.** Grey 128 gives 0.216 whether it says raw, sRGB, auto or nothing | fine for colour; **wrong for data** |
| 8-bit roughness map of grey 128 marked `raw` | behaves like roughness 0.22 (peak 0.99), not 0.5 (peak 0.06) | -> re-encoded copy, see below |
| texture transforms, alpha blend modes and cutoffs, texture coordinates, PNG/JPEG variants (ASWF assets) | the prepared file renders like the original | |
| UsdSkel **blend shapes** | not evaluated - the mesh stays in its base shape | -> baked with the skinning (checked: it moves after USDprep) |
| `Volume` with an OpenVDB field | nothing is drawn by ScanlineRender2 | reported only |

A correction that came out of this round: **DistantLight works.** The
first probe gave it intensity 3; on UsdLux's scale a distant light's
default is 50000, so 3 is darkness. At that scale Nuke lights the scene
with it, direction and falloff right. It is kept as a light now.

## Imported OBJ scans, reduced (fourth measurement)

OBJ files are converted to USD on opening (`src/core/src/ObjImport.cpp`)
and exported like any scene. Measured with a synthetic photogrammetry-like
scan - 1,000 x 1,000 quads (2 M triangles), UVs cut into 625 charts (seams
everywhere), vertex colours, one 2K texture, 3,000 floating noise
triangles - once at the origin and once at survey coordinates (4,512,000 /
210 / 5,601,000), reduced to 1/10, 1/25 and 1/100
(`tools/nuke/render_matrix.py`, `testdata/out/import_matrix`):

| Export | Nuke 17.0v1 | Nuke 16.1v4 |
|---|---|---|
| far away, full (33 MB) | textured, 1.1 s | identical pixels |
| far away, 1/10, 1/25, 1/100 (4.7 MB / 1.9 MB / 0.56 MB) | textured, no smearing at the UV seams, 0.3-0.8 s | identical pixels |
| at the origin, 1/10, 1/25, 1/100 | same pictures as far away | identical pixels |
| two objects, three materials, one with per-face materials | textured / coloured, the subsets split | identical pixels |

- Survey coordinates: the import stores points around their centre and
  puts the offset on the root as a **double** translate. Nuke renders that
  without a trace of float jitter (float points at 4.5 M would sit on a
  0.5-unit grid). The 3D view of USDprep draws in floats, so it shows such
  a scan at the origin (session layer only; the export keeps the place).
- Indexed face-varying UVs (what the import writes for charted UVs, and
  what ALab's meshes have) are read correctly.
- The meshoptimizer decimation reaches the requested share even with 625
  UV charts: 2,003,000 -> 20,029 triangles at 1/100, in about 3 s.

## What the tool does about it

The rule (Petr, 2026-09-20): stay as close as possible to what a 3D
application shows for the same file. What causes trouble in Nuke is **off
by default, behind a switch**; switched on, the tool converts or replaces
it with the closest thing Nuke reads, and **every substitution is named in
the report**. Nothing is thrown away silently.

All of it lives in one pass after the recipe (`nukeCompat`, on in the
nuke preset, off in raw/"Original", `--as-is` on the CLI;
`src/core/src/NukeCompat.cpp`, `NukeGeometry.cpp`, `UdimAtlas.cpp`). Each
row was re-rendered in Nuke 16.1 and 17.0 after the conversion.

| Nuke cannot read | Default | If kept | Verified |
|---|---|---|---|
| `.usdz` textures | output is `.usdc` + textures folder | — | ✅ |
| `<UDIM>` sets | stitched into an atlas, `UsdTransform2d` in the material | — | ✅ |
| lights (any light switches Nuke's unlit default off) | **left out** ("Include lights" / `--lights`) | distant, sphere, disk and dome lights stay lights; rect, cylinder and the rest become **axes of the same name**, position and settings still on them | ✅ |
| guide / proxy geometry (drawn on top of the real thing) | removed | kept in the file but **hidden** | ✅ |
| MaterialX output next to a standard surface (renders black) | removed with the renderer outputs | the MaterialX output alone is removed | ✅ |
| material with no standard surface (the mesh disappears) | — | the mesh is **unbound** and shows its display colour; the material stays | ✅ |
| `material:binding:preview` alone (ignored) | the chosen material is bound for every purpose | | ✅ |
| UsdSkel (bind pose) | skinning **baked into point caches**, SkelRoot becomes an Xform | | ✅ |
| `upAxis = Z` | top-level prims get `xformOp:rotateX:usdprepYUp = -90`, the stage says Y | | ✅ |
| per-face materials (GeomSubset) | the mesh becomes a **group of the same name**, each material subset a mesh of the subset's name inside it (faces in no subset: `<mesh>_rest`); UVs, per-face and per-vertex data and animation follow | | ✅ |
| Sphere, Cube, Cylinder, Cone, Capsule | turned into meshes (USD's own tessellation, 32 segments) | | ✅ |
| 8-bit data textures marked `raw` (roughness, metallic, normal maps, masks) - Nuke decodes them as sRGB | a copy with the sRGB curve applied once (`name.nukedata.png`), so Nuke's decoding lands on the stored numbers; float textures need nothing | | ✅ |
| UsdSkel blend shapes | baked into the point cache together with the skinning | | ✅ |
| Volumes (OpenVDB) | **reported only** | | - |
| BasisCurves | **reported only** — no faithful cheap substitute (ALab's stoat: 33 whisker curves) | | — |

Still open:

- De-instancing: settled by measurement - it stays the default (see the
  instancing row above and `M4_LOG.md`, slice 4).
- Curves as tubes or cards, if a show needs hair or wires in comp.
- Baking skinning evaluates the whole animation before the trim cuts it;
  fine for shots, slow for very long takes.
- Not probed yet: compressed / half-float `.tx`, RenderMan `.tex`, NURBS,
  nested instancing with material overrides, displacement.
