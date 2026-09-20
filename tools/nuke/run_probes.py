# Renders every probe of make_probes.py through GeoImport + ScanlineRender
# with one fixed camera and measures the picture: how much of the frame is
# covered, where, in which colour. Works in Nuke 16 and 17:
#   "<Nuke>.exe" -t -i tools/nuke/run_probes.py
# USDPREP_PROBES = the probe folder, USDPREP_TAG = a name for this Nuke
# (results land in <folder>/results_<tag>.json, pictures in renders_<tag>/).
import json, os, traceback
import nuke

PROBES = os.environ["USDPREP_PROBES"].replace("\\", "/")
TAG = os.environ.get("USDPREP_TAG", "nuke%d" % nuke.NUKE_VERSION_MAJOR)
RENDERS = PROBES + "/renders_" + TAG
os.makedirs(RENDERS, exist_ok=True)
manifest = json.load(open(PROBES + "/manifest.json"))
SIZE = 128
GRID = 32

nuke.addFormat("%d %d usdprep_probe" % (SIZE, SIZE))
nuke.root()["format"].setValue("usdprep_probe")


def first_class(names):
    for name in names:
        try:
            node = nuke.createNode(name, inpanel=False)
            nuke.delete(node)
            return name
        except Exception:
            pass
    return None


CAMERA = first_class(["Camera4", "Camera3", "Camera"])
LIGHT = first_class(["DirectLight1", "DirectLight", "Light4", "Light3", "Light"])
RENDER = first_class(["ScanlineRender2", "ScanlineRender"])
info = {"nuke": nuke.NUKE_VERSION_STRING, "camera": CAMERA, "light": LIGHT, "render": RENDER}
try:
    from pxr import Usd
    info["usd"] = ".".join(str(v) for v in Usd.GetVersion())
except Exception as e:
    info["usd"] = "no pxr: %s" % e
print("PROBE-INFO", json.dumps(info))

# the textures only Nuke can write for us
for kind in ("jpg", "exr", "tif"):
    target = PROBES + "/tex/blue." + kind
    if not os.path.exists(target):
        c = nuke.createNode("Constant", inpanel=False)
        c["color"].setValue([20 / 255.0, 40 / 255.0, 230 / 255.0, 1])
        c["format"].setValue("usdprep_probe")
        w = nuke.createNode("Write", inpanel=False)
        w.setInput(0, c)
        w["file"].setValue(target)
        w["file_type"].setValue({"jpg": "jpeg", "tif": "tiff"}.get(kind, kind))
        if "colorspace" in w.knobs():
            try:
                w["colorspace"].setValue("sRGB")
            except Exception:
                pass
        nuke.execute(w, 1, 1)
        nuke.delete(w); nuke.delete(c)


def measure(node, frame):
    nuke.frame(frame)
    covered = 0
    sums = [0.0, 0.0, 0.0]
    alpha = 0.0
    xs, ys = [], []
    halves = {"left": [0.0, 0.0, 0.0, 0], "right": [0.0, 0.0, 0.0, 0]}
    for j in range(GRID):
        for i in range(GRID):
            x = (i + 0.5) * SIZE / GRID
            y = (j + 0.5) * SIZE / GRID
            r, g, b, a = (node.sample(c, x, y) for c in ("red", "green", "blue", "alpha"))
            if a > 0.02 or max(r, g, b) > 0.02:
                covered += 1
                alpha += a
                for k, v in enumerate((r, g, b)):
                    sums[k] += v
                xs.append(x / SIZE); ys.append(y / SIZE)
                half = halves["left" if x < SIZE / 2 else "right"]
                for k, v in enumerate((r, g, b)):
                    half[k] += v
                half[3] += 1
    out = {"coverage": round(covered / float(GRID * GRID), 3)}
    if covered:
        out["rgb"] = [round(s / covered, 3) for s in sums]
        out["alpha"] = round(alpha / covered, 3)
        out["bbox"] = [round(min(xs), 2), round(min(ys), 2), round(max(xs), 2), round(max(ys), 2)]
        for name, half in halves.items():
            if half[3]:
                out[name] = [round(half[k] / half[3], 2) for k in range(3)]
    return out


results = {"info": info, "probes": {}}
ONLY = [x for x in os.environ.get("USDPREP_ONLY", "").split(",") if x]
if ONLY:
    try:
        results = json.load(open(PROBES + "/results_" + TAG + ".json"))
    except Exception:
        pass
    probe = nuke.createNode("GeoImport", inpanel=False)
    print("GEOIMPORT-KNOBS", sorted(probe.knobs().keys()))
    nuke.delete(probe)
for name in sorted(manifest):
    if ONLY and not any(name.startswith(x) for x in ONLY):
        continue
    spec = manifest[name]
    r = {}
    nodes = []
    try:
        geo = nuke.createNode("GeoImport", inpanel=False); nodes.append(geo)
        geo["file"].setValue(PROBES + "/" + spec["file"])
        cam = nuke.createNode(CAMERA, inpanel=False); nodes.append(cam)
        if spec.get("camera"):
            # the camera of the file: the 3D camera node reads a prim from its input
            r["camera_knobs"] = [k for k in cam.knobs() if "import" in k.lower() or "prim" in k.lower()]
            cam.setInput(0, geo)
            # Camera4 keeps the old import knobs as script-level aliases
            cam["import_chan"].setValue(True)
            cam["import_prim_path"].setValue(spec["camera"])
            cam["import_prim_path"].fromScript(spec["camera"])
        else:
            cam["translate"].setValue([0, 0, 6])
        # No Nuke light: without any light ScanlineRender shows the surface
        # colour as it is, which is what we want to measure; a file's own
        # lights do take part (the light_* probes).
        scene_input = geo
        ren = nuke.createNode(RENDER, inpanel=False); nodes.append(ren)
        # ScanlineRender2: bg, scene, camera
        ren.setInput(1, scene_input); ren.setInput(2, cam)
        w = nuke.createNode("Write", inpanel=False); nodes.append(w)
        w.setInput(0, ren)
        w["file"].setValue(RENDERS + "/" + name + ".####.png"); w["file_type"].setValue("png")
        w["channels"].setValue("rgba")
        r["frames"] = {}
        for frame in spec["frames"]:
            nuke.execute(w, frame, frame)
            r["frames"][str(frame)] = measure(ren, frame)
        if spec.get("camera"):
            r["camera_translate"] = str(cam["translate"].value())
        errors = [n.name() + ": " + n.error() for n in nodes if n.hasError()] if hasattr(geo, "error") else []
        if errors:
            r["errors"] = errors
    except Exception as e:
        r["exception"] = str(e)
        r["trace"] = traceback.format_exc()[-400:]
    for n in reversed(nodes):
        try:
            nuke.delete(n)
        except Exception:
            pass
    print("PROBE", name, json.dumps(r))
    results["probes"][name] = r
json.dump(results, open(PROBES + "/results_" + TAG + ".json", "w"), indent=1)
