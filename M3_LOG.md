# M3 — optimize & simplify: working log

**Started:** 2026-09-20 (M2 panel in daily use on ALab)

Goal from the plan: ≥70 % size / load-time reduction on the benchmark
scene, validated in Nuke. Four ops: Strip, Textures, Simplify, Trim.

## Strip, part 1: the material diet (2026-09-20)

What ALab actually looks like changed the design. Its three materials
per asset are *all* UsdPreviewSurface networks — the difference is the
**binding purpose**: `material:binding:full` → hero material with 4K
UDIM EXR sets, `material:binding:preview` → the same look with two small
JPGs. The weight sits on the purpose, not on a renderer network. So
Strip has three rules, applied on the flattened output:

1. **Purpose** — `materialPurpose: preview | full | all`. Where a prim
   binds one material per purpose, keep the one asked for and bind it
   for every purpose, so whatever Nuke asks for it gets the light one.
   Nuke preset: preview. Raw: all.
2. **Render contexts** — material outputs for a specific renderer
   (`outputs:arnold:*`, `outputs:ri:*`, `outputs:mtlx:*`) are removed,
   and with them every shader only they reached (reachability walked
   through connections from the universal outputs). Unconnected shaders
   go the same way.
3. **Unused materials** — a Material nothing binds any more (direct or
   collection binding, anywhere on the stage) is removed.

Textures only the removed shaders referenced disappear with them.

Wired through Recipe (`materialPurpose`, `stripRenderContexts`,
`stripUnusedMaterials`), the CLI (`--materials`, `--keep-render-contexts`,
`--keep-unused-materials`) and the panel (Advanced → Materials: light /
full / keep both; the preset summary names the cleanups).

**Measured, ALab stoat outfit (`/root/stoat/outfit_M_hrc`, nuke preset):**

| | before | preview | full |
|---|---|---|---|
| materials / shaders / texture refs | 3 / 16 / 9 | 1 / 4 / 2 | 1 / 9 / 6 |
| package | 91.1 MB | **71.8 MB** | 89.6 MB |

Not the 70 % yet — and the rest is not textures: the remaining 74 MB
crate holds three cloth meshes with **54–106 time samples** on points,
normals and primvars (frames 1004–1057). That is Trim's job, next.

Fixture `materials_scene.usda` (full/preview pair, a material with an
Arnold-only output, an orphan) and `test_strip` pin all three rules and
the two presets. Two older fixtures had their UDIM shader unconnected
to the surface — the reachability rule removed it, rightly; they are
wired properly now.

## Trim: the animation diet (2026-09-20)

`animation: all | range | static`, on the flattened output, at the Sdf
layer level:

- **range** — time samples outside [start, end] are erased; the stage's
  own start/end unless `frameStart`/`frameEnd` say otherwise. One
  bracketing sample is kept on each side so the boundary frames still
  interpolate. Nuke preset. Raw: all.
- **static** — one frame (`staticFrame`, default the range start) is
  read through the stage (so it interpolates where it falls between
  samples), then becomes the attribute's only value; the stage's range
  collapses to that frame.

CLI: `--animation`, `--frames a-b` (implies range), `--frame n` (implies
static). Panel: Advanced → Animation: everything / shot range (with the
scene's frames in the label) / one frame + a frame field.

**Measured, ALab stoat outfit, nuke preset (preview materials +):**

| animation | package | vs. 91.1 MB |
|---|---|---|
| all | 71.8 MB | −21 % |
| range 1004–1057 (468 pre-roll samples gone) | 60.2 MB | −34 % |
| frames 1010–1020 | 20.7 MB | −77 % |
| still, frame 1030 | 7.1 MB | **−92 %** |

So the M3 target (≥70 %) is met for a still or a short range; a full
54-frame cloth simulation stays what it is until Simplify.

Fixture `animated_scene.usda` (pre-roll from 990, samples every few
frames, a static mesh beside it) and `test_trim` pin the bracketing, the
explicit range, the interpolated still and the recipe round trip.

## Next

- Preview cards (`model:cardTexture*`) still travel into the package
  (six PNGs on the outfit): attribute-level strip.
- Textures op (OIIO) and Simplify (meshoptimizer) after that.
