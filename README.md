# usdprep — prepare USD scenes for Nuke

Small, focused utility that turns heavy production USD scenes from CG into
light, Nuke-friendly assets: extract one object, prune the rest, flatten,
package. GUI (usdtweak addon) planned on top of the same core; the CLI is
usable on farms today.

- Study & plan: `STUDY_AND_PLAN.md` · spike log: `M0_LOG.md`
- Compatibility floor: Nuke 16.0 (USD 24.05 reader) · build pin: OpenUSD 25.x
- Output profile: flattened, self-contained, UsdPreviewSurface-renderable,
  packaged `.usdc`/`.usdz` with localized (UDIM) textures

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
usdcut extract scene.usd /World/Set/Car -o car.usdz --preset nuke --report car.json
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

**Textures.** A `.usdz` output carries them inside the package. A
`.usdc`/`.usda` output copies them into a `<name>_textures` folder next to
the file and points the file at the copies, UDIM tile sets included —
keep the two together, or pass `--no-relink` to leave the paths alone.

**Materials.** Production assets often bind two materials per object: a
heavy one for final renders (`material:binding:full`, 4K UDIM sets) and a
light one for previews. `--materials preview` (what the Nuke preset does)
keeps the light one and binds it for every purpose; `full` keeps the hero
one; `all` leaves both. Render-context outputs (`outputs:arnold:*` and the
like) go with the shaders only they reach, and materials nothing binds go
too — `--keep-render-contexts` / `--keep-unused-materials` opt out.

**Filters** (`select`, `--drop-type`, `--drop-purpose`): types are schema
names (`Mesh`, `Camera`, `SphereLight`) plus the family name `light`;
purposes are `default`, `render`, `proxy`, `guide` and are resolved, so a
mesh under a guide group counts as a guide; a name without wildcards
matches anywhere in the prim name, `*` and `?` make it a wildcard match.
