# Probe: does Nuke honour UsdTransform2d? A quad with UVs 0..2 in u and a
# 2x1 atlas (left red, right green) scaled by (0.5, 1) through the node.
# Honoured: the quad is red | green. Ignored: red | green | red | green.
# Run with any python; writes into the folder given as argv[1].
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


png(os.path.join(out, "atlas.png"), 64, 32, lambda x, y: (220, 30, 30) if x < 32 else (30, 200, 30))

USDA = """#usda 1.0
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
        point3f[] points = [(-2, -1, 0), (2, -1, 0), (2, 1, 0), (-2, 1, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
        texCoord2f[] primvars:st = [(0, 0), (2, 0), (2, 1), (0, 1)] (
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
            asset inputs:file = @atlas.png@
            float2 inputs:st.connect = </Root/Mat/Xf.outputs:result>
            token inputs:wrapS = "repeat"
            token inputs:wrapT = "repeat"
            float3 outputs:rgb
        }

        def Shader "Xf"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </Root/Mat/Reader.outputs:result>
            float2 inputs:scale = (0.5, 1)
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
open(os.path.join(out, "transform2d.usda"), "w").write(USDA)
# control: the same without the transform node
open(os.path.join(out, "no_transform.usda"), "w").write(
    USDA.replace("float2 inputs:st.connect = </Root/Mat/Xf.outputs:result>",
                 "float2 inputs:st.connect = </Root/Mat/Reader.outputs:result>"))
