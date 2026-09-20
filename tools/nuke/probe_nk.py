# The .nk USDprep writes next to an .obj: load it the way an artist would
# (it brings ReadGeo nodes with their textures and a Scene), point a
# classic camera at it, render through the classic ScanlineRender.
#   "<Nuke>.exe" -t -i tools/nuke/probe_nk.py     USDPREP_NK = folder with *.nk, USDPREP_TAG
import glob, json, math, os
import nuke

FOLDER = os.environ["USDPREP_NK"].replace("\\", "/")
TAG = os.environ.get("USDPREP_TAG", "nuke%d" % nuke.NUKE_VERSION_MAJOR)
SIZE = 512
nuke.addFormat("%d %d usdprep_nk" % (SIZE, SIZE))
nuke.root()["format"].setValue("usdprep_nk")


def bounds(obj_path):
    lo, hi = [1e30] * 3, [-1e30] * 3
    for line in open(obj_path):
        if line.startswith("v "):
            p = [float(x) for x in line.split()[1:4]]
            lo = [min(a, b) for a, b in zip(lo, p)]
            hi = [max(a, b) for a, b in zip(hi, p)]
    return lo, hi


results = {}
for script in sorted(glob.glob(FOLDER + "/*.nk")):
    script = script.replace("\\", "/")
    name = os.path.splitext(os.path.basename(script))[0]
    entry = {}
    try:
        before = set(n.name() for n in nuke.allNodes())
        nuke.scriptReadFile(script)
        new = [n for n in nuke.allNodes() if n.name() not in before]
        scene = [n for n in new if n.Class() == "Scene"][0]
        entry["nodes"] = sorted(n.Class() for n in new)
        lo, hi = bounds(FOLDER + "/" + name + ".obj")
        centre = [(a + b) / 2.0 for a, b in zip(lo, hi)]
        radius = 0.5 * math.sqrt(sum((b - a) ** 2 for a, b in zip(lo, hi)))
        dist = radius * 3.2
        cam = nuke.createNode("Camera2", inpanel=False)
        offset = [dist * 0.55, dist * 0.4, dist * 0.73]
        cam["translate"].setValue([c + o for c, o in zip(centre, offset)])
        cam["rotate"].setValue([-math.degrees(math.atan2(offset[1], math.hypot(offset[0], offset[2]))),
                                math.degrees(math.atan2(offset[0], offset[2])), 0])
        cam["near"].setValue(max(0.01, dist / 1000.0)); cam["far"].setValue(dist * 10)
        ren = nuke.createNode("ScanlineRender", inpanel=False)
        ren.setInput(1, scene); ren.setInput(2, cam)
        w = nuke.createNode("Write", inpanel=False)
        w.setInput(0, ren)
        w["channels"].setValue("rgba")
        out = "%s/%s_%s.png" % (FOLDER, name, TAG)
        w["file"].setValue(out); w["file_type"].setValue("png")
        nuke.execute(w, 1, 1)
        entry["image"] = out
        entry["errors"] = [n.name() + ": " + n.error() for n in new if n.hasError()]
        for n in [w, ren, cam] + new:
            nuke.delete(n)
    except Exception as e:
        entry["exception"] = str(e)[:300]
    print("NK", name, json.dumps(entry))
    results[name] = entry
json.dump(results, open("%s/results_nk_%s.json" % (FOLDER, TAG), "w"), indent=1)
