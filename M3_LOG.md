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

## Next

- **Trim** — clip time samples to a frame range, or bake one frame.
  The outfit says everything: geometry animation is where a
  Nuke-bound asset's weight is once the hero textures are gone.
- Preview cards (`model:cardTexture*`) still travel into the package
  (six PNGs on the outfit): attribute-level strip.
- Textures op (OIIO) and Simplify (meshoptimizer) after that.
