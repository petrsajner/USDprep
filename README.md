# USDprep — prepare USD scenes for Nuke

Small, focused utility that turns heavy production USD scenes from CG into
light, Nuke-friendly assets: extract one object, prune the rest, flatten,
package. GUI (usdtweak addon) planned on top of the same core; the CLI is
usable on farms today.

- Study & plan: `STUDY_AND_PLAN.md` · spike log: `M0_LOG.md`
- Compatibility floor: Nuke 16.0 (USD 24.05 reader) · build pin: OpenUSD 25.x
- Output profile: flattened, self-contained, UsdPreviewSurface-renderable,
  `.usdc` + textures folder — the one thing Nuke reads, textures included

## Layout

```
src/core   usdprep-core — UI-free C++17 library (Inspect, Select, Extract,
           Prune, Package, recipes/presets)
src/cli    usdcut — command line face
src/addon  UsdPrep — the panel inside usdtweak (synced by tools/sync-addon.sh)
tests/     unit + golden tests with committed usda fixtures
testdata/  downloaded test scenes (gitignored)
third_party/ usdtweak clone, pixi USD env, ALab (gitignored)
```

## Build (Windows, validated toolchain)

Requires VS2019+ and an OpenUSD install. The validated path uses the
conda-forge USD from the pixi env in `third_party/usdtools`:

```bash
ENV=third_party/usdtools/.pixi/envs/default
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 \
  -DCMAKE_PREFIX_PATH="$ENV/Library/lib/cmake" \
  -Dpxr_DIR="$ENV/Library" \
  -DPython3_EXECUTABLE="$ENV/python.exe" \
  -DPython3_LIBRARY="$ENV/libs/python314.lib" \
  -DPython3_INCLUDE_DIR="$ENV/include" \
  -DPython3_VERSION=3.14
cmake --build build --config RelWithDebInfo --parallel
```

Run tests (USD DLLs must be on PATH — from the same env):

```bash
export PATH="$ENV/Library/bin:$ENV:$ENV/DLLs:$PATH"
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

## Usage

```
usdcut extract scene.usd /World/Set/Car -o car.usdc --preset nuke --report car.json
usdcut prune scene.usd --except /World/Set/Car,/World/Cameras/shotCam -o shot_min.usdc
usdcut prune scene.usd --drop /World/Lights -o no_lights.usdc
usdcut prune scene.usd --drop-type light --drop-purpose guide,proxy -o clean.usdc
usdcut select scene.usd --type Mesh --name "*door*" --topmost
usdcut inspect scene.usd --report stats.json
usdcut presets                 # what the shipped recipes do
usdcut presets nuke            # print one as JSON, the base for your own
```

Every run prints a before/after report (prims, meshes, materials, texture
refs, output size) and never modifies the input file.

**Recipes.** A recipe is every decision a run makes — de-instancing,
`defaultPrim`, texture relinking, which categories to drop. `--preset nuke`
is the shipped answer to "give me something I can drop into a comp";
`--recipe my.json` reads your own (start it from a preset and override the
two lines you care about). Flags you type win over the recipe.

**Only what Nuke reads.** What Nuke 16 and 17 read was measured with 67
probe scenes (`NUKE_COMPAT.md`), and the tool offers nothing they cannot
load. The output is a `.usdc` (or `.usda`); a `.usdz` is refused — Nuke
loads a package's geometry and none of its textures. What causes trouble
is off by default and behind a switch; switched on, it is converted or
replaced and named in the report, never dropped silently: UDIM sets
become one atlas texture, lights Nuke cannot use become axes of the same
name (`--lights` brings lights along at all), kept guide/proxy geometry is
hidden, skinning is baked to point caches, Z-up scenes are stood up,
per-face materials become one mesh per material, implicit shapes become
meshes. `--as-is` skips these conversions, `--keep-udim` the atlas. Other
applications are served by stock usdtweak or by building on
`usdprep-core`, which still packages.

**Older Nuke (classic 3D).** An older Nuke has only the classic ReadGeo,
which does not read USD the way 14+ does. `-o car.abc` and `-o car.obj`
write what it does read: meshes in world space with the UVs the material
uses - an `.abc` with the animation (the trimmed range, or one frame with
`--frame`), an `.obj` as a still. Next to the file: the textures folder,
one file per material in `car_abc_parts/` when there are several (a ReadGeo
takes one texture for all it reads), and `car_abc.nk` - File > Insert Comp
Nodes brings ReadGeo nodes with their textures wired in and a Scene
joining them, because Nuke reads no materials from either format. Set the
Nuke project to the scene's frame rate for an `.abc`. Everything the USD
export does applies first: one material per mesh, skinning baked, shapes
meshed, UDIM atlases with their UVs baked into the file. `.abc` needs the
Alembic library at build time (`third_party/alembic-install`, see
`M4_LOG.md`); without it the rest builds and an `.abc` output says so.

**Textures.** They are copied into a `<name>_textures` folder next to the
file, and the file points at the copies (single-tile UDIM sets become the
tile itself) — keep the two together, or pass `--no-relink` to leave the
paths alone.

**Materials.** Production assets often bind two materials per object: a
heavy one for final renders (`material:binding:full`, 4K UDIM sets) and a
light one for previews. `--materials preview` (what the Nuke preset does)
keeps the light one and binds it for every purpose; `full` keeps the hero
one; `all` leaves both. Render-context outputs (`outputs:arnold:*` and the
like) go with the shaders only they reach, and materials nothing binds go
too — `--keep-render-contexts` / `--keep-unused-materials` opt out. The
draw-mode card setup (six preview textures per asset that Nuke never
draws) goes as well; `--keep-cards` keeps it.

**Texture size.** `--max-texture 4096` (the Nuke preset) scales any texture
larger than that on its longer side down — in the output only, the
originals stay as they are; PNG, JPEG and EXR, UDIM sets tile by tile.
`0` leaves every texture alone.

**Geometry.** `--simplify 0.25` decimates dense meshes to a quarter of
their triangles, keeping UVs and normals as well as it can (the result is
triangles, per-vertex attributes, no subdivision). Never on by itself.

**Everything above is a switch.** Each reduction has its own flag, the
`raw` preset ("Original" in the panel) turns all of them off, and the
panel's Advanced section shows every one as a checkbox or a choice —
the original is always one click away.

**Animation.** `--animation range` (the Nuke preset) drops time samples
outside the scene's start/end — a simulation's pre-roll, typically —
keeping one bracketing sample on each side; `--frames 1010-1020` picks the
range yourself; `--frame 1030` bakes a still and drops the animation;
`--animation all` leaves every sample alone.

**Filters** (`select`, `--drop-type`, `--drop-purpose`): types are schema
names (`Mesh`, `Camera`, `SphereLight`) plus the family name `light`;
purposes are `default`, `render`, `proxy`, `guide` and are resolved, so a
mesh under a guide group counts as a guide; a name without wildcards
matches anywhere in the prim name, `*` and `?` make it a wildcard match.
