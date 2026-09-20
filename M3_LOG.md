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

## Strip, part 2: draw-mode cards (2026-09-20)

Every ALab asset carries the UsdGeomModelAPI card setup — six PNGs on
`model:cardTexture*`, `drawMode = inherited`, `applyDrawMode = false` —
so the cards are never drawn and the textures travel for nothing. The
whole draw-mode family of attributes goes (`stripDrawModeCards`, nuke
preset on, raw off, `--keep-cards` to keep it). The geometry is right
there; Nuke draws that.

**ALab projector, nuke preset: 4.56 MB → 686 KB** — one preview JPG
inside the package instead of six card PNGs and a UDIM set.

## Textures: the resolution cap (2026-09-20)

`maxTextureSize` (nuke preset 4096, raw 0): a texture larger than that
on its longer side is read through USD's own Hio, area-averaged down,
written back in the same format into the scratch folder, and the
flattened layer repointed at the copy — the packager or the localizer
then take the copy, the original is never touched. UDIM sets go tile by
tile and stay a set (tiles under the cap are copied as they are next to
the scaled ones). Formats Hio cannot read are left alone and named.
CLI `--max-texture <px>`; panel: Advanced → Textures (8K / 4K / 2K / 1K /
512 / no cap).

**ALab stoat outfit, full-quality materials, still frame 1030** (the
EXR UDIM tiles are 1024 px):

| cap | package |
|---|---|
| none / 1024 | 24.4 MB |
| 512 | 9.6 MB — 12 EXR tiles at 512×512 |

So the M3 "≥70 %" holds for the hero-material case too, with a cap.
What this cannot do yet: `.tex/.tx/.rat` (no reader without OpenImageIO)
and format conversion (EXR → JPEG for previews) — both wait for the
release build with OIIO.

Fixture `bigtex_scene.usda` (a 256×128 PNG, a 200×100 UDIM pair, the
1×1 checker) and `test_textures` pin the scaling, the untouched
originals, the package and the presets.

## Simplify: decimation (2026-09-20)

meshoptimizer 0.23 (MIT) is vendored verbatim under
`src/third_party/meshoptimizer` and built as a static library the core
links privately. `simplifyRatio` (0 = as it is; **no preset turns it
on**): every mesh with ≥ 500 faces is decimated to that share of its
triangles.

How a USD mesh gets there and back:

1. Every per-corner attribute the mesh has — primvars flattened through
   their indices, plus `normals` — is expanded to floats per corner.
   Float-family arrays only; per-face (`uniform`) values and anything
   else are dropped and named. A single value under a per-vertex label
   is treated as the constant it is.
2. One **render vertex** per unique (point, attribute values) pair, so a
   UV seam or a hard edge is a split vertex, the way a GPU sees it.
   meshoptimizer welds positions for its own adjacency and keeps the
   seams where they are.
3. Faces are fan-triangulated (holes left out), decimated with
   `meshopt_simplifyWithAttributes` (UV weight 0.5, normals 0.3, others
   0.15, at most 32 attribute floats), no error ceiling — the ratio is
   the contract.
4. Collapses land on existing vertices, so the result is a vertex
   subset plus a new triangle list. Every time sample of points and of
   every carried attribute is remapped through that subset — animation
   survives.
5. Written back as triangles with every attribute vertex-interpolated,
   extent recomputed per sample, subdivision scheme `none`, creases /
   corners / holes gone. Meshes with per-face material subsets or
   animated topology are left alone and reported.

**ALab, the heaviest asset (`decor_choko_experiment01_0001`, 59 meshes,
still frame):**

| ratio | triangles | package | time |
|---|---|---|---|
| as it is | 714,506 | 13.1 MB | — |
| 0.25 | 178,607 (24 %) | 4.8 MB | 1.7 s |
| 0.10 | 71,404 (10 %) | 2.8 MB | — |

Fixture `dense_scene.usda` (a 40×40 grid with a UV seam and vertex
normals, a pebble below the minimum) and `test_simplify` pin the
triangle count, the per-vertex UVs/normals, the untouched small mesh,
the off-by-default and the presets.

## Every reduction is a switch

Asked for and now true everywhere: each reduction — materials purpose,
renderer networks, unused materials, preview cards, guide/proxy
geometry, texture cap, animation, decimation — is its own recipe key,
its own CLI flag and its own checkbox or choice in the panel's
Advanced section. The `raw` preset (shown as "Original (nothing
changed)") turns all of them off; the panel's switches override
whatever preset is chosen, both ways.

## Nuke 17 validation — the M3 exit (2026-09-20)

Driven headless: `Nuke17.0.exe -t -i` (the interactive licence; `-t`
alone asks for a render licence this machine does not have) with
`tools/nuke/load_matrix.py` and `tools/nuke/render_matrix.py`. Eleven
exports of ALab assets (original / nuke preset / trimmed / still / hero
materials with a cap / decimated / the whole stoat), each opened
through GeoImport and through Nuke's own USD 25.08, then rendered
through ScanlineRender2 with a camera framed on the bounding box.

**Every file opens without error in Nuke's USD 25.08**, frame ranges,
defaultPrim and materials as written. Then the renders:

| | .usdz | .usdc + textures folder |
|---|---|---|
| geometry | ✅ | ✅ |
| textures | ❌ black — `Read error` for every texture inside the package | ✅ |
| multi-tile `<UDIM>` sets | ❌ black | ❌ black — Nuke does not expand the template |
| decimated (0.25 / 0.10) | — | ✅ looks right, textures intact |
| trimmed animation, mid-frame | — | ✅ |
| whole stoat, 261 meshes | — | ✅ 1.8 s for two frames |

Two facts that change the product:

1. **Nuke 17 does not read textures from inside a .usdz.** The
   panel's recommended format is now the `.usdc` + `_textures` folder
   ("what Nuke reads"); `.usdz` stays for other applications and the
   report says so out loud when one is written.
2. **Nuke 17 does not expand `<UDIM>`.** A single-tile set is now
   rewritten to its tile (Nuke reads that); a multi-tile set is named
   in the report as a material that will render black. The nuke
   preset's preview materials use plain JPGs and are unaffected — one
   more reason it is the default.

Render times, two frames each: original jar 1.95 s, nuke preset 1.2 s,
decimated to a tenth 0.5–0.7 s. Load itself is lazy in Nuke; the
render is the honest number.

## Only what Nuke reads (product rule, after the validation)

Petr's rule: the tool must not offer anything Nuke cannot read — a
misleading option is the user's lost time. Whoever needs another
application uses stock usdtweak or builds on the core. So:

- The panel has one format, `.usdc` + textures folder; the format combo
  and the `.usdz` save filter are gone. `usdcut` refuses a `.usdz`
  output with the reason. The core library still packages (tests cover
  it) — that is the "build it yourself" path.
- "Full quality" materials: where the hero material uses a multi-tile
  UDIM set and the object also has a light one, the light one is bound
  and the report says so (`nuke` warning). With no light material to
  fall back to, the black-material warning stays. Fixture
  `udim_hero_scene.usda`.
- Superseded for the usual case by the UDIM atlas below; the fallback
  only acts when the atlas is switched off.

## UDIM atlas

Nuke 17 does not expand `<UDIM>`, but it does honour `UsdTransform2d`
(probe: `tools/nuke/make_transform2d_probe.py`, rendered red|green at
the middle of the quad with the node, at a quarter without). So the fix
stays inside the material (`src/core/src/UdimAtlas.cpp`):

- every multi-tile set a `UsdUVTexture` reads is stitched into one image
  laid out like the UV space it covers (`name.atlas.ext`, missing cells
  black, same pixel format as the tiles);
- a `UsdTransform2d` goes between the texture and whatever fed its `st`
  (a primvar reader is added when nothing did):
  `scale = 1/(columns, rows)`, `translation = -(minColumn, minRow)/(columns, rows)`;
- meshes and their UVs are not touched, so shared UV sets, subsets and
  animated UVs are no concern.

The texture cap applies per tile (a 4K cap on a 2x1 set gives an
8192x4096 atlas), and an atlas never exceeds 16384 px. The tiles
themselves are no longer shipped. Switch: `udimAtlas` in the recipe
(nuke: on, raw: off), `--keep-udim` on the CLI; the panel has no
checkbox for it - only "Original" leaves a set alone.

Checked in Nuke 17: the fixture (`udim_atlas_scene.usda`, tiles 1002,
1003, 1012) lands every colour where its tile was, and ALab's outfit
with `--materials full` (6 sets x 2 EXR tiles) renders textured where it
was black before. Export 1.6 s.

Limits: only `UsdUVTexture` is rewired (a renderer-specific image node
on a UDIM set stays and is warned about); tiles of one set must share a
pixel format; Nuke 16 has not been checked for `UsdTransform2d` yet.

## What the environment does not have

- **OpenImageIO** and **meshoptimizer** are not in the conda USD env.
- USD's own `Hio` reads and writes PNG/JPG/EXR (and resamples on read),
  so a **resolution cap** needs no new dependency. `.tex/.tx/.rat`
  conversion does, and waits for the release build of USD with OIIO
  (M4).
- meshoptimizer is vendored now (see Simplify above).

## Next

