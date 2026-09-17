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
src/core   usdprep-core — UI-free C++17 library (Inspect, Extract, Prune, Package)
src/cli    usdcut — command line face
tests/     unit tests + committed usda fixtures
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
usdcut extract scene.usd /World/Set/Car -o car.usdz --report car.json
usdcut prune scene.usd --except /World/Set/Car,/World/Cameras/shotCam -o shot_min.usdc
usdcut prune scene.usd --drop /World/Lights -o no_lights.usdc
usdcut inspect scene.usd --report stats.json
```

Every run prints a before/after report (prims, meshes, materials, texture
refs, output size) and never modifies the input file.
