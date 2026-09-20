# What does Nuke read? One tiny scene per question, each built so that the
# answer shows in a render as a colour, a coverage or a position - things
# run_probes.py measures without anybody looking at pictures.
#
#   <pixi env>/python.exe tools/nuke/make_probes.py <out-folder>
#
# Every scene is looked at by the same camera: at (0, 0, 6), looking down
# -Z, so a 2x2 quad in the XY plane fills about two thirds of the frame.
import json, os, struct, sys, zlib

from pxr import Sdf, Usd, UsdUtils

OUT = sys.argv[1]
TEX = os.path.join(OUT, "tex")
os.makedirs(TEX, exist_ok=True)
manifest = {}


def png(path, width, height, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


BLUE = (20, 40, 230)
png(os.path.join(TEX, "blue.png"), 32, 32, lambda x, y: BLUE)
png(os.path.join(TEX, "udim.1001.png"), 32, 32, lambda x, y: BLUE)
png(os.path.join(TEX, "udim.1002.png"), 32, 32, lambda x, y: BLUE)
png(os.path.join(TEX, "solo.1001.png"), 32, 32, lambda x, y: BLUE)
png(os.path.join(TEX, "atlas.png"), 64, 32, lambda x, y: (230, 20, 20) if x < 32 else (20, 230, 20))
# grey ramp for an opacity texture: left transparent, right opaque
png(os.path.join(TEX, "mask.png"), 32, 32, lambda x, y: (0, 0, 0) if x < 16 else (255, 255, 255))
# blue.jpg / blue.exr / blue.tif are written by Nuke itself (run_probes.py)

HEADER = '''#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Y"%s
)
'''


def quad(name="Quad", binding="</Root/Mat>", extra="", size=1.0, st="[(0, 0), (1, 0), (1, 1), (0, 1)]", order="[0, 1, 2, 3]"):
    bind = ("        rel material:binding = %s\n" % binding) if binding else ""
    api = '(\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )' if binding else ""
    return '''    def Mesh "%s" %s
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = %s
        point3f[] points = [(-%g, -%g, 0), (%g, -%g, 0), (%g, %g, 0), (-%g, %g, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = %s (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
%s%s    }
''' % (name, api, order, size, size, size, size, size, size, size, size, st, bind, extra)


def material(name="Mat", surface_inputs="color3f inputs:diffuseColor = (0.9, 0.05, 0.05)", extra_shaders="",
             outputs=None):
    outputs = outputs or 'token outputs:surface.connect = </Root/%s/Surface.outputs:surface>' % name
    return '''    def Material "%s"
    {
        %s

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            %s
            token outputs:surface
        }
%s    }
''' % (name, outputs, surface_inputs, extra_shaders)


def textured(file, name="Mat", st_source=None, extra=""):
    st_source = st_source or "</Root/%s/Reader.outputs:result>" % name
    return material(name, "color3f inputs:diffuseColor.connect = </Root/%s/Tex.outputs:rgb>" % name, '''
        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @%s@
            float2 inputs:st.connect = %s
            float3 outputs:rgb
            float outputs:r
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
%s''' % (file, st_source, extra))


def scene(body, meta=""):
    return HEADER % meta + 'def Xform "Root"\n{\n' + body + "}\n"


def probe(name, text, question, expect, light=True, frames=(1,), ext="usda", files=None):
    path = os.path.join(OUT, name + ".usda")
    with open(path, "w") as f:
        f.write(text)
    for extra_name, extra_text in (files or {}).items():
        with open(os.path.join(OUT, extra_name), "w") as f:
            f.write(extra_text)
    final = name + ".usda"
    if ext == "usdc":
        Sdf.Layer.FindOrOpen(path).Export(os.path.join(OUT, name + ".usdc"))
        os.remove(path)
        final = name + ".usdc"
    elif ext == "usdz":
        UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(path), os.path.join(OUT, name + ".usdz"))
        os.remove(path)
        final = name + ".usdz"
    manifest[name] = {"file": final, "question": question, "expect": expect, "light": light, "frames": list(frames)}


RED, GREEN = "(0.9, 0.05, 0.05)", "(0.05, 0.9, 0.05)"

# ---------------------------------------------------------------- formats
for ext in ("usda", "usdc", "usdz"):
    probe("fmt_" + ext, scene(quad() + textured("tex/blue.png")), "textured quad as ." + ext, "blue", ext=ext)

# --------------------------------------------------------------- textures
for kind in ("jpg", "exr", "tif"):
    probe("tex_" + kind, scene(quad() + textured("tex/blue." + kind)), kind + " texture", "blue")
probe("tex_udim_multi", scene(quad() + textured("tex/udim.<UDIM>.png")), "<UDIM> template, two tiles", "blue if read")
probe("tex_udim_single", scene(quad() + textured("tex/solo.<UDIM>.png")), "<UDIM> template, one tile", "blue if read")
probe("tex_transform2d",
      scene(quad(st="[(0, 0), (2, 0), (2, 1), (0, 1)]") + textured("tex/atlas.png", st_source="</Root/Mat/Xf.outputs:result>", extra='''
        def Shader "Xf"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </Root/Mat/Reader.outputs:result>
            float2 inputs:scale = (0.5, 1)
            float2 outputs:result
        }
''')), "UsdTransform2d (what the UDIM atlas relies on)", "left red, right green")
probe("tex_abs_missing", scene(quad() + textured("tex/does_not_exist.png")), "missing texture", "anything but a crash")

# -------------------------------------------------------------- materials
probe("mat_constant", scene(quad() + material()), "UsdPreviewSurface, constant colour", "red")
probe("mat_displaycolor", scene(quad(binding=None, extra='        color3f[] primvars:displayColor = [(0.05, 0.9, 0.05)]\n')),
      "no material, displayColor", "green")
probe("mat_emissive", scene(quad() + material(surface_inputs="color3f inputs:diffuseColor = (0, 0, 0)\n            color3f inputs:emissiveColor = (0.05, 0.05, 0.9)")),
      "emissiveColor", "blue", light=False)
probe("mat_opacity", scene(quad() + material(surface_inputs="color3f inputs:diffuseColor = (0.9, 0.05, 0.05)\n            float inputs:opacity = 0.3")),
      "opacity 0.3", "alpha around 0.3")
probe("mat_opacity_tex", scene(quad() + textured("tex/mask.png").replace(
    "color3f inputs:diffuseColor.connect = </Root/Mat/Tex.outputs:rgb>",
    "color3f inputs:diffuseColor = (0.9, 0.05, 0.05)\n            float inputs:opacity.connect = </Root/Mat/Tex.outputs:r>")),
      "opacity from a texture", "left half transparent")
probe("mat_inherited", scene('''    def Xform "Group" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </Root/Mat>
''' + quad(binding=None).replace("\n    ", "\n        ") + "    }\n" + material()), "binding inherited from the parent", "red")
probe("mat_both_purposes", scene(quad(binding=None, extra='''        rel material:binding:full = </Root/Full>
        rel material:binding:preview = </Root/Preview>
''').replace('def Mesh "Quad" ', 'def Mesh "Quad" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )') +
    material("Full", "color3f inputs:diffuseColor = " + RED) + material("Preview", "color3f inputs:diffuseColor = " + GREEN)),
      "full (red) and preview (green) bindings - which one does Nuke show", "red or green")
for purpose in ("full", "preview"):
    probe("mat_only_" + purpose, scene(quad(binding=None, extra="        rel material:binding:%s = </Root/Mat>\n" % purpose).replace(
        'def Mesh "Quad" ', 'def Mesh "Quad" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )') + material()),
          "only a material:binding:%s" % purpose, "red if honoured")
probe("mat_subsets", scene('''    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4]
        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
        point3f[] points = [(-1, -1, 0), (0, -1, 0), (1, -1, 0), (-1, 1, 0), (0, 1, 0), (1, 1, 0)]
        uniform token subdivisionScheme = "none"

        def GeomSubset "Left" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </Root/RedMat>
        }

        def GeomSubset "Right" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </Root/GreenMat>
        }
    }
''' + material("RedMat", "color3f inputs:diffuseColor = " + RED) + material("GreenMat", "color3f inputs:diffuseColor = " + GREEN)),
      "per-face materials through GeomSubsets", "left red, right green")
MTLX = '''
        def Shader "Mtlx"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.05, 0.9, 0.05)
            float inputs:base = 1
            token outputs:out
        }
'''
probe("mat_mtlx_only", scene(quad() + material(extra_shaders=MTLX, outputs="token outputs:mtlx:surface.connect = </Root/Mat/Mtlx.outputs:out>")),
      "MaterialX standard_surface only (green)", "green if read")
probe("mat_mtlx_and_preview", scene(quad() + material(extra_shaders=MTLX, outputs='''token outputs:surface.connect = </Root/Mat/Surface.outputs:surface>
        token outputs:mtlx:surface.connect = </Root/Mat/Mtlx.outputs:out>''')),
      "UsdPreviewSurface (red) next to MaterialX (green)", "red or green")
probe("mat_arnold_only", scene(quad() + material(extra_shaders='''
        def Shader "Ai"
        {
            uniform token info:id = "arnold:standard_surface"
            color3f inputs:base_color = (0.05, 0.9, 0.05)
            token outputs:out
        }
''', outputs="token outputs:arnold:surface.connect = </Root/Mat/Ai.outputs:out>")), "renderer-only material (arnold)", "not green")

# --------------------------------------------------------------- geometry
probe("geo_tris", scene(quad(order="[0, 1, 2, 3]").replace("faceVertexCounts = [4]", "faceVertexCounts = [3, 3]").replace(
    "faceVertexIndices = [0, 1, 2, 3]", "faceVertexIndices = [0, 1, 2, 0, 2, 3]") + material()), "triangles", "red, same coverage as a quad")
probe("geo_ngon", scene('''    def Mesh "Ngon" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [6]
        int[] faceVertexIndices = [0, 1, 2, 3, 4, 5]
        point3f[] points = [(1, 0, 0), (0.5, 0.87, 0), (-0.5, 0.87, 0), (-1, 0, 0), (-0.5, -0.87, 0), (0.5, -0.87, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </Root/Mat>
    }
''' + material()), "a six-sided face", "red hexagon")
CUBE = '''    def Mesh "Cube" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 4, 6, 7, 5, 0, 4, 5, 1, 2, 3, 7, 6, 0, 2, 6, 4, 1, 5, 7, 3]
        point3f[] points = [(-1, -1, -1), (1, -1, -1), (-1, 1, -1), (1, 1, -1), (-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1)]
        uniform token subdivisionScheme = "%s"
        rel material:binding = </Root/Mat>
    }
'''
probe("geo_subdiv_none", scene(CUBE % "none" + material()), "cube, no subdivision", "red square")
probe("geo_subdiv_cc", scene(CUBE % "catmullClark" + material()), "cube, catmullClark", "smaller than geo_subdiv_none if subdivided")
probe("geo_lefthanded", scene(quad(extra='        uniform token orientation = "leftHanded"\n') + material()), "leftHanded orientation", "red or culled")
probe("geo_backface", scene(quad(order="[3, 2, 1, 0]") + material()), "single-sided quad facing away", "culled or red")
probe("geo_backface_ds", scene(quad(order="[3, 2, 1, 0]", extra="        uniform bool doubleSided = 1\n") + material()),
      "doubleSided quad facing away", "red")
for shape, attrs in (("Sphere", "double radius = 1"), ("Cube", "double size = 2"), ("Cylinder", "double radius = 1\n        double height = 2"),
                     ("Cone", "double radius = 1\n        double height = 2"), ("Capsule", "double radius = 0.5\n        double height = 1")):
    probe("geo_" + shape.lower(), scene('''    def %s "Shape" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        %s
        rel material:binding = </Root/Mat>
    }
''' % (shape, attrs) + material()), "implicit " + shape, "red shape")
probe("geo_curves", scene('''    def BasisCurves "Curves"
    {
        int[] curveVertexCounts = [2, 2]
        point3f[] points = [(-1, -1, 0), (1, 1, 0), (-1, 1, 0), (1, -1, 0)]
        uniform token type = "linear"
        float[] widths = [0.2] (
            interpolation = "constant"
        )
        color3f[] primvars:displayColor = [(0.05, 0.9, 0.05)]
    }
'''), "BasisCurves", "green cross if drawn")
probe("geo_points", scene('''    def Points "Pts"
    {
        point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
        float[] widths = [0.5, 0.5, 0.5, 0.5]
        color3f[] primvars:displayColor = [(0.05, 0.9, 0.05)]
    }
'''), "Points", "green dots if drawn")
probe("geo_invisible", scene(quad(extra='        token visibility = "invisible"\n') + material()), "visibility = invisible", "nothing")
for purpose in ("render", "proxy", "guide"):
    probe("geo_purpose_" + purpose, scene(quad(extra='        uniform token purpose = "%s"\n' % purpose) + material()),
          "purpose = " + purpose, "drawn or not")
probe("geo_inactive", scene(quad().replace('def Mesh "Quad" (', 'def Mesh "Quad" (\n        active = false') + material()),
      "active = false", "nothing")

# ------------------------------------------------ instancing, composition
probe("inst_native", HEADER % "" + '''class Xform "_Proto"
{
''' + quad(size=0.5) + '''}

def Xform "Root"
{
    def Xform "A" (
        instanceable = true
        references = </_Proto>
    )
    {
        double3 xformOp:translate = (-0.8, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Xform "B" (
        instanceable = true
        references = </_Proto>
    )
    {
        double3 xformOp:translate = (0.8, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' + material() + "}\n", "native instancing (instanceable references)", "two red squares")
probe("inst_point", scene('''    def PointInstancer "Scatter"
    {
        point3f[] positions = [(-1, 0, 0), (0, 0, 0), (1, 0, 0)]
        int[] protoIndices = [0, 0, 0]
        rel prototypes = </Root/Scatter/Protos/Quad>

        def Scope "Protos"
        {
''' + quad(size=0.3).replace("\n    ", "\n            ").replace("    def Mesh", "            def Mesh", 1) + '''        }
    }
''' + material()), "PointInstancer", "three red squares")
probe("comp_reference", scene('''    def "Ref" (
        references = @./comp_reference_asset.usda@
    )
    {
    }
'''), "an unflattened reference to another file", "red",
      files={"comp_reference_asset.usda": HEADER % "" + 'def Xform "Root"\n{\n' + quad() + material() + "}\n"})
probe("comp_variant", HEADER % "" + '''def Xform "Root" (
    variants = {
        string look = "green"
    }
    prepend variantSets = "look"
)
{
''' + quad() + '''    variantSet "look" = {
        "green" {
''' + material(surface_inputs="color3f inputs:diffuseColor = " + GREEN).replace("\n    ", "\n            ").replace("    def Material", "            def Material", 1) + '''        }
        "red" {
''' + material().replace("\n    ", "\n            ").replace("    def Material", "            def Material", 1) + '''        }
    }
}
''', "variant selection (green selected)", "green")

# -------------------------------------------------------------- animation
probe("anim_xform", scene('''    def Xform "Mover"
    {
        double3 xformOp:translate.timeSamples = {
            1: (-0.8, 0, 0),
            10: (0.8, 0, 0),
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
''' + quad(size=0.4).replace("\n    ", "\n        ").replace("    def Mesh", "        def Mesh", 1) + "    }\n" + material(),
      meta="\n    startTimeCode = 1\n    endTimeCode = 10\n    timeCodesPerSecond = 24"),
      "animated transform", "left at frame 1, right at frame 10", frames=(1, 10))
probe("anim_points", scene(quad(size=0.4).replace(
    "point3f[] points = [(-0.4, -0.4, 0), (0.4, -0.4, 0), (0.4, 0.4, 0), (-0.4, 0.4, 0)]",
    '''point3f[] points.timeSamples = {
            1: [(-1.2, -0.4, 0), (-0.4, -0.4, 0), (-0.4, 0.4, 0), (-1.2, 0.4, 0)],
            10: [(0.4, -0.4, 0), (1.2, -0.4, 0), (1.2, 0.4, 0), (0.4, 0.4, 0)],
        }''') + material(), meta="\n    startTimeCode = 1\n    endTimeCode = 10\n    timeCodesPerSecond = 24"),
      "animated points (a point cache)", "left at frame 1, right at frame 10", frames=(1, 10))
probe("anim_visibility", scene(quad(extra='''        token visibility.timeSamples = {
            1: "inherited",
            10: "invisible",
        }
''') + material(), meta="\n    startTimeCode = 1\n    endTimeCode = 10"), "animated visibility", "there at 1, gone at 10", frames=(1, 10))
probe("anim_fps_offset", scene('''    def Xform "Mover"
    {
        double3 xformOp:translate.timeSamples = {
            1001: (-0.8, 0, 0),
            1010: (0.8, 0, 0),
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
''' + quad(size=0.4).replace("\n    ", "\n        ").replace("    def Mesh", "        def Mesh", 1) + "    }\n" + material(),
      meta="\n    startTimeCode = 1001\n    endTimeCode = 1010"),
      "animation at production frame numbers", "left at 1001, right at 1010", frames=(1001, 1010))

# ---------------------------------------------------------- stage metadata
BOX = '''    def Mesh "Box" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 4, 6, 7, 5, 0, 4, 5, 1, 2, 3, 7, 6, 0, 2, 6, 4, 1, 5, 7, 3]
        point3f[] points = [(-0.3, -0.3, -1), (0.3, -0.3, -1), (-0.3, 0.3, -1), (0.3, 0.3, -1), (-0.3, -0.3, 1), (0.3, -0.3, 1), (-0.3, 0.3, 1), (0.3, 0.3, 1)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </Root/Mat>
    }
'''
probe("meta_upz", scene(BOX + material()).replace('upAxis = "Y"', 'upAxis = "Z"'),
      "upAxis = Z: a box that is tall along Z", "tall if Nuke converts the axis, small square if not")
probe("meta_mpu1", scene(quad(size=0.5) + material(), meta="\n    metersPerUnit = 1"),
      "metersPerUnit = 1, quad of 1 unit", "same size as meta_mpu001 unless Nuke scales by units")
probe("meta_mpu001", scene(quad(size=0.5) + material(), meta="\n    metersPerUnit = 0.01"), "metersPerUnit = 0.01, same quad", "reference")

# ------------------------------------------------------------------ lights
WHITE = material(surface_inputs="color3f inputs:diffuseColor = (0.8, 0.8, 0.8)")
probe("light_none", scene(quad() + WHITE), "no light anywhere", "black or headlight - the baseline", light=False)
probe("light_usd_distant", scene(quad() + WHITE + '''    def DistantLight "Sun"
    {
        color3f inputs:color = (1, 0.1, 0.1)
        float inputs:intensity = 3
    }
'''), "a UsdLux DistantLight (red) in the file", "red if the file's lights are used", light=False)
probe("light_usd_sphere", scene(quad() + WHITE + '''    def SphereLight "Bulb"
    {
        color3f inputs:color = (0.1, 1, 0.1)
        float inputs:intensity = 50
        float inputs:radius = 0.2
        double3 xformOp:translate = (0, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''), "a UsdLux SphereLight (green) in the file", "green if the file's lights are used", light=False)

# ------------------------------------------------------------------ camera
probe("cam_usd", scene(quad(size=0.5) + material() + '''    def Camera "Cam"
    {
        float focalLength = 50
        float horizontalAperture = 24.576
        float verticalAperture = 18.672
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (1, 0, 6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''), "a Camera in the file, one unit to the right", "quad left of centre when rendered through it")
manifest["cam_usd"]["camera"] = "/Root/Cam"

# ------------------------------------------------ second round: follow-ups
probe("mat_arnold_and_preview", scene(quad() + material(extra_shaders="""
        def Shader "Ai"
        {
            uniform token info:id = "arnold:standard_surface"
            color3f inputs:base_color = (0.05, 0.9, 0.05)
            token outputs:out
        }
""", outputs="""token outputs:surface.connect = </Root/Mat/Surface.outputs:surface>
        token outputs:arnold:surface.connect = </Root/Mat/Ai.outputs:out>""")), "UsdPreviewSurface (red) next to an arnold output", "red")
probe("mat_no_outputs", scene(quad() + '    def Material "Mat"\n    {\n    }\n'), "bound to a material with no outputs at all", "grey, or the mesh vanishes")
probe("mat_unbound", scene(quad(binding=None)), "no material, no displayColor", "default grey")
for kind, attrs in (("DistantLight", "float inputs:angle = 1"), ("SphereLight", "float inputs:radius = 0.3"),
                    ("RectLight", "float inputs:width = 1\n        float inputs:height = 1"), ("DiskLight", "float inputs:radius = 0.5"),
                    ("CylinderLight", "float inputs:radius = 0.2\n        float inputs:length = 1"), ("DomeLight", "")):
    probe("light_" + kind.lower(), scene(quad() + WHITE + """    def %s "L"
    {
        color3f inputs:color = (0.1, 1, 0.1)
        float inputs:intensity = 30
        %s
        double3 xformOp:translate = (0, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
""" % (kind, attrs)), kind + " (green) in the file", "green if this light type works; black if it only switches the default lighting off", light=False)
probe("skel_anim", HEADER % "\n    startTimeCode = 1\n    endTimeCode = 10" + """def SkelRoot "Root"
{
    def Skeleton "Skel" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        uniform token[] joints = ["Base", "Base/Tip"]
        uniform matrix4d[] bindTransforms = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)), ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))]
        uniform matrix4d[] restTransforms = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)), ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))]
        rel skel:animationSource = </Root/Skel/Anim>

        def SkelAnimation "Anim"
        {
            uniform token[] joints = ["Base", "Base/Tip"]
            float3[] translations.timeSamples = {
                1: [(0, 0, 0), (-0.8, 0, 0)],
                10: [(0, 0, 0), (0.8, 0, 0)],
            }
            quatf[] rotations = [(1, 0, 0, 0), (1, 0, 0, 0)]
            half3[] scales = [(1, 1, 1), (1, 1, 1)]
        }
    }

    def Mesh "Quad" (
        prepend apiSchemas = ["SkelBindingAPI", "MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.4, -0.4, 0), (0.4, -0.4, 0), (0.4, 0.4, 0), (-0.4, 0.4, 0)]
        uniform token subdivisionScheme = "none"
        int[] primvars:skel:jointIndices = [1, 1, 1, 1] (
            elementSize = 1
            interpolation = "vertex"
        )
        float[] primvars:skel:jointWeights = [1, 1, 1, 1] (
            elementSize = 1
            interpolation = "vertex"
        )
        matrix4d primvars:skel:geomBindTransform = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))
        rel skel:skeleton = </Root/Skel>
        rel material:binding = </Root/Mat>
    }
""" + material() + "}\n", "UsdSkel skinning (a joint carries the quad)", "left at 1, right at 10 if skinning is evaluated; centre if not", frames=(1, 10))

json.dump(manifest, open(os.path.join(OUT, "manifest.json"), "w"), indent=1)
print(len(manifest), "probes in", OUT)
