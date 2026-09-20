# Level-3 formats: does Nuke's 3D system read .abc, .obj and .fbx the way it
# reads .usd? Nuke writes the files itself (classic Sphere -> WriteGeo), then
# each is read back through GeoImport (the USD-based 3D system) and through
# the classic ReadGeo, rendered and measured.
#   "<Nuke>.exe" -t -i tools/nuke/probe_formats.py      USDPREP_PROBES, USDPREP_TAG as in run_probes.py
import json, os, traceback
import nuke

PROBES = os.environ["USDPREP_PROBES"].replace("\\", "/")
TAG = os.environ.get("USDPREP_TAG", "nuke%d" % nuke.NUKE_VERSION_MAJOR)
OUT = PROBES + "/formats"
os.makedirs(OUT, exist_ok=True)
SIZE, GRID = 128, 24
nuke.addFormat("%d %d usdprep_fmt" % (SIZE, SIZE))
nuke.root()["format"].setValue("usdprep_fmt")


def make(names):
    for name in names:
        try:
            return nuke.createNode(name, inpanel=False)
        except Exception:
            pass
    return None


def coverage(node):
    nuke.frame(1)
    hits = 0
    for j in range(GRID):
        for i in range(GRID):
            x, y = (i + 0.5) * SIZE / GRID, (j + 0.5) * SIZE / GRID
            if node.sample("alpha", x, y) > 0.02 or node.sample("red", x, y) > 0.02:
                hits += 1
    return round(hits / float(GRID * GRID), 3)


results = {"nuke": nuke.NUKE_VERSION_STRING, "written": {}, "read": {}}

# ---- write: a classic sphere into each format
for ext in ("abc", "obj", "fbx"):
    target = "%s/sphere_%s.%s" % (OUT, TAG, ext)
    try:
        sphere = make(["Sphere"])
        writer = make(["WriteGeo"])
        writer.setInput(0, sphere)
        writer["file"].setValue(target)
        nuke.execute(writer, 1, 1)
        results["written"][ext] = os.path.exists(target) and os.path.getsize(target)
        nuke.delete(writer); nuke.delete(sphere)
    except Exception as e:
        results["written"][ext] = "failed: %s" % e

# ---- read each back, both ways
for ext in ("abc", "obj", "fbx"):
    target = "%s/sphere_%s.%s" % (OUT, TAG, ext)
    if not os.path.exists(target):
        continue
    for reader_class, render_classes in (("GeoImport", ["ScanlineRender2"]), ("ReadGeo2", ["ScanlineRender"]),
                                         ("ReadGeo", ["ScanlineRender"])):
        key = "%s via %s" % (ext, reader_class)
        nodes = []
        try:
            reader = nuke.createNode(reader_class, inpanel=False); nodes.append(reader)
            reader["file"].setValue(target)
            cam = make(["Camera4"] if reader_class == "GeoImport" else ["Camera2", "Camera3", "Camera"]); nodes.append(cam)
            cam["translate"].setValue([0, 0, 4])
            ren = make(render_classes); nodes.append(ren)
            ren.setInput(1, reader); ren.setInput(2, cam)
            w = nuke.createNode("Write", inpanel=False); nodes.append(w)
            w.setInput(0, ren)
            w["file"].setValue("%s/%s_%s_%s.png" % (OUT, TAG, ext, reader_class)); w["file_type"].setValue("png")
            nuke.execute(w, 1, 1)
            results["read"][key] = {"coverage": coverage(ren),
                                    "errors": [n.name() + ": " + n.error() for n in nodes if n.hasError()]}
        except Exception as e:
            results["read"][key] = {"exception": str(e)[:200]}
        for n in reversed(nodes):
            try:
                nuke.delete(n)
            except Exception:
                pass
        print("FORMAT", key, json.dumps(results["read"][key]))
print("FORMATS", json.dumps(results))
json.dump(results, open("%s/results_%s.json" % (OUT, TAG), "w"), indent=1)
