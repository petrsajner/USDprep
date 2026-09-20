# Nuke validation: open every .usdz in a folder through GeoImport and through
# Nuke's own pxr (USD 25.08), record errors, prim/material/texture counts and
# the frame range. Run with the interactive licence:
#   "C:/Program Files/Nuke17.0v1/Nuke17.0.exe" -t -i tools/nuke/load_matrix.py
import nuke, os, time, glob, json, sys
MATRIX = r"C:\Users\Petr\Documents\USD\testdata\out\nuke_matrix"
OUT = os.path.join(MATRIX, "nuke_results.json")
results = []
print("NUKE", nuke.NUKE_VERSION_STRING)
try:
    from pxr import Usd, UsdGeom, UsdShade
    print("PXR", Usd.GetVersion())
    have_pxr = True
except Exception as e:
    print("PXR unavailable:", e); have_pxr = False

probe = nuke.createNode("GeoImport", inpanel=False)
print("GeoImport knobs:", sorted(probe.knobs().keys()))
nuke.delete(probe)

for path in sorted(glob.glob(os.path.join(MATRIX, "*.usdz"))):
    name = os.path.basename(path)
    r = {"file": name, "size_mb": round(os.path.getsize(path) / 1e6, 1)}
    t0 = time.time()
    n = nuke.createNode("GeoImport", inpanel=False)
    n["file"].setValue(path.replace("\\", "/"))
    try:
        n.forceValidate()
    except Exception as e:
        r["validate_error"] = str(e)
    # push the scene through a node that has to evaluate it
    try:
        xf = nuke.createNode("GeoTransform", inpanel=False)
        xf.setInput(0, n)
        xf.forceValidate()
        for f in range(1004, 1010):
            nuke.frame(f); xf.forceValidate()
        nuke.delete(xf)
    except Exception as e:
        r["eval_error"] = str(e)
    r["load_s"] = round(time.time() - t0, 2)
    r["nuke_error"] = n.error() if n.hasError() else ""
    for k in ("scene_graph", "sceneGraph", "scene_view", "prims"):
        if k in n.knobs():
            try:
                items = n[k].getAllItems()
                r["scene_items"] = len(items); r["scene_knob"] = k
            except Exception as e:
                r["scene_items_error"] = str(e)
            break
    if have_pxr:
        try:
            t1 = time.time()
            stage = Usd.Stage.Open(path)
            prims = meshes = mats = tex = 0
            for p in Usd.PrimRange(stage.GetPseudoRoot(), Usd.TraverseInstanceProxies(Usd.PrimDefaultPredicate)):
                if p.IsPseudoRoot(): continue
                prims += 1
                if p.GetTypeName() == "Mesh": meshes += 1
                if p.GetTypeName() == "Material": mats += 1
                if p.GetTypeName() == "Shader":
                    for a in p.GetAttributes():
                        if a.GetTypeName() == "asset" and a.Get() and a.Get().path: tex += 1
            r["pxr"] = {"open_s": round(time.time() - t1, 2), "prims": prims, "meshes": meshes, "materials": mats,
                        "texture_refs": tex, "frames": [stage.GetStartTimeCode(), stage.GetEndTimeCode()],
                        "defaultPrim": stage.GetDefaultPrim().GetName() if stage.GetDefaultPrim() else ""}
        except Exception as e:
            r["pxr_error"] = str(e)
    nuke.delete(n)
    results.append(r)
    print("RESULT", json.dumps(r))
json.dump(results, open(OUT, "w"), indent=1)
print("WROTE", OUT)
