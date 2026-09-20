# Level-3 formats: does Nuke's 3D system read .abc, .obj and .fbx the way it
# reads .usd? Nuke writes the files itself (classic Sphere -> WriteGeo), then
# each is read back through GeoImport (the USD-based 3D system) and through
# the classic ReadGeo, rendered and measured.
#   "<Nuke>.exe" -t -i tools/nuke/probe_formats.py      USDPREP_PROBES, USDPREP_TAG as in run_probes.py
import glob, json, os, traceback
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

# ---- read each back: through GeoImport (the USD-based 3D system) and
#      through the classic ReadGeo + ScanlineRender, the only 3D there is
#      in older Nukes. The classic rig gets a checkerboard as the texture,
#      so a picture with two tones in it means the UVs arrived as well.
def classic_reader(path):
    reader = nuke.createNode("ReadGeo2", inpanel=False)
    reader["file"].setValue(path)
    # (an .abc created from a script loads all its items; touching scene_view here unloads them)
    if path.endswith(".fbx") and "all_objects" in reader.knobs():
        reader["all_objects"].setValue(True)  # .fbx: every object, not the first one
    return reader


def tones(node):
    nuke.frame(1)
    values = set()
    for j in range(GRID):
        for i in range(GRID):
            x, y = (i + 0.5) * SIZE / GRID, (j + 0.5) * SIZE / GRID
            if node.sample("alpha", x, y) > 0.5:
                values.add(round(node.sample("red", x, y), 1))
    return sorted(values)


for path in sorted(glob.glob(OUT + "/*.abc") + glob.glob(OUT + "/*.obj") + glob.glob(OUT + "/*.fbx")):
    path = path.replace("\\", "/")
    name = os.path.basename(path)
    if name.startswith("sphere_") and TAG not in name:
        continue
    for system in ("classic", "geoimport"):
        key = "%s via %s" % (name, system)
        nodes = []
        try:
            if system == "classic":
                checker = nuke.createNode("CheckerBoard2", inpanel=False); nodes.append(checker)
                reader = classic_reader(path); nodes.append(reader)
                reader.setInput(0, checker)
                cam = nuke.createNode("Camera2", inpanel=False); nodes.append(cam)
                ren = nuke.createNode("ScanlineRender", inpanel=False); nodes.append(ren)
            else:
                reader = nuke.createNode("GeoImport", inpanel=False); nodes.append(reader)
                reader["file"].setValue(path)
                cam = nuke.createNode("Camera4", inpanel=False); nodes.append(cam)
                ren = nuke.createNode("ScanlineRender2", inpanel=False); nodes.append(ren)
            cam["translate"].setValue([0, 0, 6])
            ren.setInput(1, reader); ren.setInput(2, cam)
            w = nuke.createNode("Write", inpanel=False); nodes.append(w)
            w.setInput(0, ren)
            w["channels"].setValue("rgba")
            w["file"].setValue("%s/%s_%s_%s.####.png" % (OUT, TAG, name.replace(".", "_"), system)); w["file_type"].setValue("png")
            entry = {}
            for frame in (1, 10):
                nuke.execute(w, frame, frame)
                nuke.frame(frame)
                entry["f%d" % frame] = {"coverage": coverage(ren)}
            entry["tones"] = tones(ren) if system == "classic" else None
            entry["errors"] = [n.name() + ": " + n.error() for n in nodes if n.hasError()]
            results["read"][key] = entry
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
