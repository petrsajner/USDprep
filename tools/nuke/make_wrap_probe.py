# Probe: does Nuke's USD reader repeat a texture outside 0..1? One quad per
# file, a 2x2 texture (red, green / blue, yellow):
#   wrap_uv3        UVs 0..3, wrapS/T = repeat     -> 3x3 tiles when repeated
#   wrap_xform3     UVs 0..1, UsdTransform2d scale 3, repeat
#   wrap_mirror     UVs 0..3, mirror
#   wrap_unset      UVs 0..3, no wrap set
#   wrap_inside     UVs 0..1, a texture already tiled 3x3 (what a baked copy does)
#   wrap_chain      the tiled texture through two UsdTransform2d in a row
#                   (scale 3, then 1/3): 3x3 tiles when Nuke follows both
# Clamped instead: one big tile and the edge texels smeared over the rest.
# Run with any python; writes into the folder given as argv[1]; render the
# folder with render_matrix.py (USDPREP_MATRIX).
import os, struct, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def png(path, width, height, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def quadrant(x, y, size):
    # PNG rows run top down; v = 0 is the bottom row
    left, bottom = x < size // 2, y >= size // 2
    if bottom:
        return (220, 30, 30) if left else (30, 200, 30)
    return (30, 60, 220) if left else (230, 220, 30)


png(os.path.join(out, "quads.png"), 64, 64, lambda x, y: quadrant(x, y, 64))
png(os.path.join(out, "quads3x3.png"), 192, 192, lambda x, y: quadrant(x % 64, y % 64, 64))

TEMPLATE = """#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Y"
)
def Xform "Root"
{
    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
        texCoord2f[] primvars:st = [(0, 0), (%(uv)s, 0), (%(uv)s, %(uv)s), (0, %(uv)s)] (
            interpolation = "vertex"
        )
        rel material:binding = </Root/Mat>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Root/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </Root/Mat/Tex.outputs:rgb>
            token outputs:surface
        }

        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @%(file)s@
            float2 inputs:st.connect = </Root/Mat/%(st)s.outputs:result>
%(wrap)s            float3 outputs:rgb
        }

        def Shader "Xf"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </Root/Mat/Reader.outputs:result>
            float2 inputs:scale = (%(scale)s, %(scale)s)
            float2 inputs:translation = (0, 0)
            float2 outputs:result
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
    }
}
"""


def wrap(mode):
    return "" if not mode else '            token inputs:wrapS = "%s"\n            token inputs:wrapT = "%s"\n' % (mode, mode)


CASES = {
    "wrap_uv3": dict(uv=3, file="quads.png", st="Reader", wrap=wrap("repeat"), scale=1),
    "wrap_xform3": dict(uv=1, file="quads.png", st="Xf", wrap=wrap("repeat"), scale=3),
    "wrap_mirror": dict(uv=3, file="quads.png", st="Reader", wrap=wrap("mirror"), scale=1),
    "wrap_unset": dict(uv=3, file="quads.png", st="Reader", wrap=wrap(""), scale=1),
    "wrap_inside": dict(uv=1, file="quads3x3.png", st="Reader", wrap=wrap("repeat"), scale=1),
}
for name, values in CASES.items():
    with open(os.path.join(out, name + ".usda"), "w") as f:
        f.write(TEMPLATE % values)

chain = TEMPLATE % dict(uv=1, file="quads3x3.png", st="Xf2", wrap=wrap("repeat"), scale=3)
chain = chain.replace('''        def Shader "Reader"''', '''        def Shader "Xf2"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </Root/Mat/Xf.outputs:result>
            float2 inputs:scale = (0.3333333, 0.3333333)
            float2 inputs:translation = (0, 0)
            float2 outputs:result
        }

        def Shader "Reader"''')
with open(os.path.join(out, "wrap_chain.usda"), "w") as f:
    f.write(chain)
print("wrote", len(CASES) + 1, "probes into", out)
