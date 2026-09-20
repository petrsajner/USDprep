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
| native instancing (`instanceable`) | ✅ | de-instancing is not needed for Nuke |
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
| SphereLight, DiskLight, DomeLight in the file | used — and the unlit default is gone | a dome at production intensity blows out |
| DistantLight in the file | counts as a light, contributes next to nothing → **black render** | |
| RectLight, CylinderLight | ignored altogether | |
| Camera in the file | ✅ through the Camera node's import (translate, focal) | |

## What this means for the tool

Already right: `.usdc` only, the UDIM atlas, single-tile rewrite,
binding the chosen material for every purpose.

Gaps, in the order they bite:

1. **Guide/proxy geometry and renderer material outputs must go, always.**
   Today they are switches a user can turn off; turned off, Nuke shows the
   asset doubled, or black. They belong with the atlas: not a choice in
   the panel.
2. **A material with nothing Nuke can render makes the mesh vanish.**
   After the strip, such a material should be unbound (the mesh then
   shows its displayColor or grey) and named in the report.
3. **UsdSkel** — characters that arrive skinned stand in their bind pose.
   USD ships `UsdSkelBakeSkinning`; baking to a point cache on export
   makes them move.
4. **GeomSubset materials** — one mesh, several materials: Nuke shows
   grey. Fix: split the mesh by its material subsets.
5. **`upAxis = Z`** scenes (Houdini, Blender, 3ds Max pipelines) arrive
   lying down. Fix: rotate the root and declare Y.
6. **Implicit shapes** and **BasisCurves** are not drawn. Shapes can be
   meshed on export; curves can only be reported (or tubed — expensive).
7. **Lights**: a file's lights switch Nuke's unlit default off, and a
   DistantLight alone renders black. For "drop it into a comp" the nuke
   preset should leave lights out by default (the option exists:
   drop type `light`); cameras are fine and worth keeping.
8. **De-instancing is not needed for Nuke** and is the size trap on big
   sets; it could default to off when nothing inside an instance has to
   be edited (strip/atlas inside prototypes needs a look first).

Not probed yet: normal/roughness/metallic maps (they need a lit render to
judge), `.tx/.tex` textures, texture colour spaces, volumes, NURBS,
blend shapes, nested instancing with material overrides.
