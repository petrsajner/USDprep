# Third round of probes: what Nuke does with the shading inputs beyond the
# diffuse colour (roughness, metallic, normal maps - these need a light to
# show), with a texture's colour space, and with blend shapes. Adds to the
# folder and manifest make_probes.py wrote; run_probes.py measures them.
#   python tools/nuke/make_probes_shading.py <probe-folder>
import json, os, struct, sys, zlib

OUT = sys.argv[1]
TEX = os.path.join(OUT, "tex")
manifest = json.load(open(os.path.join(OUT, "manifest.json")))


def png(path, width, height, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


png(os.path.join(TEX, "grey128.png"), 8, 8, lambda x, y: (128, 128, 128))
# a normal map that leans every normal towards +X: n = (0.6, 0, 0.8)
png(os.path.join(TEX, "normal_lean_x.png"), 8, 8, lambda x, y: (204, 128, 230))
# roughness: left half mirror-like, right half chalk
png(os.path.join(TEX, "rough_lr.png"), 32, 8, lambda x, y: (20, 20, 20) if x < 16 else (240, 240, 240))

HEAD = '''#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Y"%s
)
'''
QUAD = '''    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"%s]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </Root/Mat>
%s    }
'''
# the light sits where the camera is (0, 0, 6): the highlight of a mirror-like surface lands mid-quad
LIGHT = '''    def SphereLight "L"
    {
        float inputs:intensity = %g
        float inputs:radius = 0.3
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''


def material(inputs, shaders=""):
    return '''    def Material "Mat"
    {
        token outputs:surface.connect = </Root/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            %s
            token outputs:surface
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
%s    }
''' % (inputs, shaders)


def texture(name, file, colour_space=None, extra=""):
    space = ('            token inputs:sourceColorSpace = "%s"\n' % colour_space) if colour_space else ""
    return '''
        def Shader "%s"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @%s@
            float2 inputs:st.connect = </Root/Mat/Reader.outputs:result>
%s%s            float3 outputs:rgb
            float outputs:r
        }
''' % (name, file, space, extra)


def probe(name, text, question, expect, frames=(1,)):
    open(os.path.join(OUT, name + ".usda"), "w").write(text)
    manifest[name] = {"file": name + ".usda", "question": question, "expect": expect, "light": False,
                      "frames": list(frames)}


def scene(quad_extra, mat, light=None, api=""):
    body = QUAD % (api, quad_extra) + mat + (LIGHT % light if light else "")
    return HEAD % "" + 'def Xform "Root"\n{\n' + body + "}\n"


GREY = "color3f inputs:diffuseColor = (0.5, 0.5, 0.5)"
FRONT = (60, 0, 0, 6)
SIDE = (60, 4, 0, 2)   # from the +X side: a normal leaning towards +X catches more of it

for rough in (0.1, 0.5, 0.9):
    probe("shade_rough_%02d" % int(rough * 100),
          scene("", material(GREY + "\n            float inputs:roughness = %g" % rough), FRONT),
          "roughness %g under a light at the camera" % rough, "the lower the roughness, the higher the peak")
probe("shade_rough_tex",
      scene("", material(GREY + "\n            float inputs:roughness.connect = </Root/Mat/Rough.outputs:r>",
                         texture("Rough", "tex/rough_lr.png", "raw")), FRONT),
      "roughness from a texture: left smooth, right rough", "left and right halves differ")
for metal in (0, 1):
    probe("shade_metal_%d" % metal,
          scene("", material("color3f inputs:diffuseColor = (0.9, 0.3, 0.05)\n            float inputs:roughness = 0.4"
                             "\n            float inputs:metallic = %d" % metal), FRONT),
          "metallic %d, orange base colour" % metal, "metal: darker diffuse, tinted highlight")
probe("shade_normal_none", scene("", material(GREY + "\n            float inputs:roughness = 1"), SIDE),
      "no normal map, light from the +X side", "reference")
probe("shade_normal_map",
      scene("", material(GREY + "\n            float inputs:roughness = 1"
                                "\n            normal3f inputs:normal.connect = </Root/Mat/Normal.outputs:rgb>",
                         texture("Normal", "tex/normal_lean_x.png", "raw",
                                 "            float4 inputs:scale = (2, 2, 2, 1)\n"
                                 "            float4 inputs:bias = (-1, -1, -1, 0)\n")), SIDE),
      "normal map leaning the normals towards the light", "brighter than shade_normal_none if the map is used")
for space in ("raw", "sRGB", "auto", None):
    probe("tex_space_%s" % (space or "omitted"),
          scene("", material("color3f inputs:diffuseColor.connect = </Root/Mat/Tex.outputs:rgb>",
                             texture("Tex", "tex/grey128.png", space))),
          "8-bit grey 128, sourceColorSpace = %s" % (space or "(not authored)"),
          "raw: 0.50, sRGB: 0.22")

# --- blend shapes: the weight slides the quad from left to right
probe("skel_blendshape", HEAD % "\n    startTimeCode = 1\n    endTimeCode = 10" + '''def SkelRoot "Root"
{
    def Skeleton "Skel" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        uniform token[] joints = ["Base"]
        uniform matrix4d[] bindTransforms = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))]
        uniform matrix4d[] restTransforms = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))]
        rel skel:animationSource = </Root/Skel/Anim>

        def SkelAnimation "Anim"
        {
            uniform token[] joints = ["Base"]
            float3[] translations = [(0, 0, 0)]
            quatf[] rotations = [(1, 0, 0, 0)]
            half3[] scales = [(1, 1, 1)]
            uniform token[] blendShapes = ["slide"]
            float[] blendShapeWeights.timeSamples = {
                1: [0],
                10: [1],
            }
        }
    }

    def Mesh "Quad" (
        prepend apiSchemas = ["SkelBindingAPI", "MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1.2, -0.4, 0), (-0.4, -0.4, 0), (-0.4, 0.4, 0), (-1.2, 0.4, 0)]
        uniform token subdivisionScheme = "none"
        int[] primvars:skel:jointIndices = [0, 0, 0, 0] (
            elementSize = 1
            interpolation = "vertex"
        )
        float[] primvars:skel:jointWeights = [1, 1, 1, 1] (
            elementSize = 1
            interpolation = "vertex"
        )
        uniform token[] skel:blendShapes = ["slide"]
        rel skel:blendShapeTargets = </Root/Quad/slide>
        rel skel:skeleton = </Root/Skel>
        rel material:binding = </Root/Mat>

        def BlendShape "slide"
        {
            uniform vector3f[] offsets = [(1.6, 0, 0), (1.6, 0, 0), (1.6, 0, 0), (1.6, 0, 0)]
            uniform int[] pointIndices = [0, 1, 2, 3]
        }
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Root/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.9, 0.05, 0.05)
            token outputs:surface
        }
    }
}
''', "a blend shape slides the quad from left (frame 1) to right (frame 10)",
      "moves if blend shapes are evaluated; stays left if not", frames=(1, 10))

json.dump(manifest, open(os.path.join(OUT, "manifest.json"), "w"), indent=1)
print(len(manifest), "probes in the manifest")
