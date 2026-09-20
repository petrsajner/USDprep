# Material / texture behaviour in Nuke, on the ASWF USD working group's test
# assets (third_party/usd-wg-assets, Apache-2.0: normal maps, roughness,
# colour spaces, texture file formats, alpha modes, texture transforms).
# For each scene two files land in the output folder, both lit by the same
# sphere light so that normals and roughness show:
#   <name>__orig.usda   the asset as delivered
#   <name>__prep.usda   the same asset after `usdcut extract --preset nuke`
# tools/nuke/render_matrix.py then renders the folder in Nuke.
#   <pixi env>/python.exe tools/nuke/make_material_tests.py <out-folder>
import os, subprocess, sys

from pxr import Gf, Usd, UsdGeom

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(ROOT, "third_party", "usd-wg-assets", "test_assets")
USDCUT = os.path.join(ROOT, "build", "src", "cli", "RelWithDebInfo", "usdcut.exe")
OUT = os.path.abspath(sys.argv[1])
os.makedirs(os.path.join(OUT, "exports"), exist_ok=True)

SCENES = {
    "normals": "NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda",
    "roughness": "RoughnessTest/RoughnessTest.usdz",
    "colorspace": "ColorSpaceTests/UsdPreviewSurface/usduvtexture_color_test.usda",
    "fileformats": "TextureFileFormatTests/all_files.usda",
    "alphablend": "AlphaBlendModeTest/AlphaBlendModeTest.usd",
    "textransform": "TextureTransformTest/TextureTransformTest.usd",
    "texcoord": "TextureCoordinateTest/TextureCoordinateTest.usda",
}

WRAPPER = '''#usda 1.0
(
    subLayers = [@%(layer)s@]
    upAxis = "%(up)s"
    metersPerUnit = %(mpu)g
)

def SphereLight "usdprepTestLight"
{
    float inputs:intensity = 220
    float inputs:radius = %(radius)g
    double3 xformOp:translate = (%(x)g, %(y)g, %(z)g)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
'''


def wrap(layer_path, out_path):
    stage = Usd.Stage.Open(layer_path)
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.render])
    box = cache.ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedBox()
    centre = (box.GetMin() + box.GetMax()) / 2.0
    size = (box.GetMax() - box.GetMin()).GetLength() or 1.0
    up = UsdGeom.GetStageUpAxis(stage)
    # up and to the front-left of the camera render_matrix.py uses
    offset = Gf.Vec3d(-0.5, 0.9, 1.0) if up == "Y" else Gf.Vec3d(-0.5, -1.0, 0.9)
    pos = centre + offset * size
    open(out_path, "w").write(WRAPPER % {"layer": layer_path.replace("\\", "/"), "up": up,
                                         "mpu": UsdGeom.GetStageMetersPerUnit(stage), "radius": size * 0.1,
                                         "x": pos[0], "y": pos[1], "z": pos[2]})


for name, relative in SCENES.items():
    source = os.path.join(ASSETS, relative)
    if not os.path.exists(source):
        print("MISSING", source)
        continue
    wrap(source, os.path.join(OUT, name + "__orig.usda"))
    stage = Usd.Stage.Open(source)
    roots = [str(p.GetPath()) for p in stage.GetPseudoRoot().GetChildren()]
    export = os.path.join(OUT, "exports", name + ".usdc")
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    run = subprocess.run([USDCUT, "extract", source] + roots + ["-o", export, "--preset", "nuke", "--report",
                                                                 os.path.join(OUT, "exports", name + ".json")],
                         capture_output=True, text=True, env=env)
    notes = [line.strip() for line in run.stdout.splitlines() if "[warning]" in line or "] nuke" in line or
             "] textures" in line]
    print("==", name, "exit", run.returncode)
    for line in notes[:8]:
        print("   ", line[:200])
    if os.path.exists(export):
        wrap(export, os.path.join(OUT, name + "__prep.usda"))
