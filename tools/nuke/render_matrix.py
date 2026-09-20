# Nuke validation: render every .usdz/.usdc in a folder through GeoImport +
# ScanlineRender2 with a camera framed on the bounding box, one or two
# frames each, and time it. Run with the interactive licence:
#   "C:/Program Files/Nuke17.0v1/Nuke17.0.exe" -t -i tools/nuke/render_matrix.py
# with USDPREP_MATRIX pointing at the folder (default: testdata/out/nuke_matrix).
# Results land in <folder>/renders/ as PNGs plus render_results.json.
import nuke, os, time, glob, json, math
from pxr import Usd, UsdGeom, Gf
MATRIX = os.environ.get("USDPREP_MATRIX", "C:/Users/Petr/Documents/USD/testdata/out/nuke_matrix")
RENDERS = os.path.join(MATRIX, "renders"); os.makedirs(RENDERS, exist_ok=True)

def make(cls):
    try:
        return nuke.createNode(cls, inpanel=False)
    except Exception as e:
        return None
# node class discovery, once
classes = {}
for role, names in {"camera": ["Camera4", "Camera3", "Camera"], "light": ["Light4", "Light3", "Light", "DirectLight"],
                    "render": ["ScanlineRender2", "ScanlineRender"], "write": ["Write"]}.items():
    for nm in names:
        n = make(nm)
        if n:
            classes[role] = nm
            print("CLASS", role, nm, "knobs:", [k for k in n.knobs().keys() if any(s in k.lower() for s in ["translate", "rotate", "focal", "haperture", "file", "format", "sample", "output", "projection", "channels", "type", "intensity"])][:16])
            nuke.delete(n); break
print("CLASSES", classes)

def frame_camera(path):
    stage = Usd.Stage.Open(path)
    bbox = UsdGeom.BBoxCache(stage.GetStartTimeCode(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.render]).ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedBox()
    c = (bbox.GetMin() + bbox.GetMax()) / 2.0; size = (bbox.GetMax() - bbox.GetMin()).GetLength()
    return c, size, stage.GetStartTimeCode(), stage.GetEndTimeCode()

results = []
for path in sorted(glob.glob(os.path.join(MATRIX, "*.usd[zca]"))):
    name = os.path.splitext(os.path.basename(path))[0]
    r = {"file": name}
    try:
        c, size, f0, f1 = frame_camera(path)
        geo = nuke.createNode("GeoImport", inpanel=False); geo["file"].setValue(path.replace("\\", "/"))
        cam = nuke.createNode(classes["camera"], inpanel=False)
        dist = size * 1.4
        cam["translate"].setValue([c[0] + dist * 0.6, c[1] + dist * 0.35, c[2] + dist * 0.75])
        # look at the centre: yaw/pitch from the offset
        dx, dy, dz = dist * 0.6, dist * 0.35, dist * 0.75
        yaw = math.degrees(math.atan2(dx, dz)); pitch = -math.degrees(math.atan2(dy, math.hypot(dx, dz)))
        cam["rotate"].setValue([pitch, yaw, 0])
        light = nuke.createNode(classes["light"], inpanel=False) if "light" in classes else None
        if light:
            light["translate"].setValue([c[0] + dist, c[1] + dist, c[2] + dist])
            if "intensity" in light.knobs(): light["intensity"].setValue(1.0)
        ren = nuke.createNode(classes["render"], inpanel=False)
        ren.setInput(0, light if light else geo); ren.setInput(1, geo); ren.setInput(2, cam)
        if "samples" in ren.knobs(): ren["samples"].setValue(1)
        w = nuke.createNode("Write", inpanel=False); w.setInput(0, ren)
        out = os.path.join(RENDERS, name + ".####.png").replace("\\", "/")
        w["file"].setValue(out); w["file_type"].setValue("png")
        mid = int((f0 + f1) / 2)
        frames = [int(f0), mid] if f1 > f0 else [int(f0)]
        t0 = time.time()
        for f in frames:
            nuke.execute(w, f, f)
        r["render_s"] = round(time.time() - t0, 2); r["frames"] = frames
        r["error"] = ren.error() if ren.hasError() else (geo.error() if geo.hasError() else "")
        r["bbox_size"] = round(size, 2)
        for n in (w, ren, cam, geo) + ((light,) if light else ()): nuke.delete(n)
    except Exception as e:
        r["exception"] = str(e)
    print("RENDER", json.dumps(r)); results.append(r)
json.dump(results, open(os.path.join(RENDERS, "render_results.json"), "w"), indent=1)
