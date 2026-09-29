#!/usr/bin/env python3
"""Phase 2 on the current Resolve project: set the seed and request detect+track through the
plugin's inputs (by INPS_ID), render the timeline, then report the sidecar track."""
import os, sys, json, time, signal, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve_api import connect, wait_render

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "resolve_render")
TAG = os.environ.get("PM_TAG", time.strftime("%H%M%S"))


def step(name, fn, timeout=120):
    print("step:", name, flush=True)
    signal.alarm(timeout)
    r = fn()
    signal.alarm(0)
    print("   ->", r if isinstance(r, (bool, int, float, str, list, dict, type(None))) else type(r).__name__, flush=True)
    return r


resolve = step("connect", connect, 200)
proj = resolve.GetProjectManager().GetCurrentProject()
tl = proj.GetCurrentTimeline()
ti = tl.GetItemListInTrack("video", 1)[0]
step("fusion page", lambda: resolve.OpenPage("fusion"))
comp = ti.GetFusionCompByIndex(1)
tools = comp.GetToolList(False) or {}
plate = next((t for t in tools.values() if "PlateMask" in str(t.GetAttrs().get("TOOLS_RegID"))), None)
if not plate:
    raise SystemExit("plate tool missing")
inputs = plate.GetInputList() or {}
by_id = {}
for inp in inputs.values():
    a = inp.GetAttrs()
    by_id[a.get("INPS_ID")] = (inp, a.get("INPS_Name"), a.get("INPS_DataType"))
print("inputs:", {k: (v[1], v[2]) for k, v in by_id.items()}, flush=True)


def set_input(iid, value, t=None):
    if iid not in by_id:
        print("   (no input %s)" % iid, flush=True)
        return None
    inp = by_id[iid][0]
    return inp.SetSource(value, t) if False else (plate.SetInput(iid, value) if t is None else plate.SetInput(iid, value, t))


# Show the node in a Fusion viewer and select it so the host exercises the overlay interact.
step("select tool", lambda: comp.SetActiveTool(plate))
step("view on viewer 1", lambda: comp.CurrentFrame.ViewOn(plate, 1))
step("set comp time 10", lambda: comp.SetAttrs({"COMPN_CurrentTime": 10}))
time.sleep(3)
seed_x, seed_y = 565.0 / 960.0, 270.0 / 540.0
seed_id = next((k for k in by_id if k and "seed" in k.lower()), None)
print("seed input id:", seed_id, "current:", plate.GetInput(seed_id) if seed_id else None, flush=True)
for val in ([seed_x, seed_y], {1: seed_x, 2: seed_y}, {1: seed_x, 2: seed_y, 3: 0.0}):
    step("seed %r" % (val,), lambda v=val: set_input(seed_id, v))
    rb = plate.GetInput(seed_id)
    print("   readback:", rb, flush=True)
    if rb and abs(float(rb[1]) - seed_x) < 1e-3:
        break


def find(name):
    return next((k for k in by_id if k and k.lower() == name.lower()), None)


for name, val in [("notifyWhenDone", int(os.environ.get("PM_NOTIFY", "0"))), ("trackingMode", int(os.environ.get("PM_MODE", "0"))), ("requestPlate", 0), ("requestFrame", 10), ("detectRequest", 1), ("trackRequest", 1), ("blockSize", 10), ("expand", 4), ("feather", 3)]:
    iid = find(name)
    if name.endswith("Request") and iid:  # counters: always one above the current value so a re-run re-triggers
        val = int(float(plate.GetInput(iid) or 0)) + 1
    step("%s=%s (%s)" % (name, val, iid), lambda i=iid, v=val: set_input(i, v))
    if iid:
        print("   readback:", plate.GetInput(iid), flush=True)

t0 = time.time()
last = -1
while time.time() - t0 < 90:
    time.sleep(2)
    tracks = sorted(glob.glob(os.path.expanduser("~/Library/Application Support/PlateMask/tracks/*.json")), key=os.path.getmtime)
    if not tracks:
        continue
    st0 = json.load(open(tracks[-1]))
    n = len(st0["plates"][0].get("keys", []))
    if n != last:
        print("  t+%.0fs auto-track: keys=%d live=%s comp time=%s" % (time.time() - t0, n, st0["plates"][0].get("live"), comp.GetAttrs().get("COMPN_CurrentTime")), flush=True)
        last = n
    if not st0["plates"][0].get("live") and n > 1:
        break
print("auto-track result:", st0["status"] if tracks else None, flush=True)
step("deliver page", lambda: resolve.OpenPage("deliver"))
settings = {"TargetDir": OUT, "CustomName": "platemask_" + TAG, "SelectAllFrames": True, "ExportAudio": False}
step("render settings", lambda: proj.SetRenderSettings(settings))
job = step("add render job", lambda: proj.AddRenderJob())
step("start render", lambda: proj.StartRendering(job, isInteractiveMode=False))
status = step("wait render", lambda: wait_render(proj, job, int(os.environ.get("PM_WAIT", "1200"))), timeout=int(os.environ.get("PM_WAIT", "1200")) + 30)
print("render status:", status, flush=True)
print("plugin status param:", plate.GetInput("status"), flush=True)
files = sorted(glob.glob(os.path.join(OUT, "platemask_%s*" % TAG)))
print("output:", files, flush=True)
tracks = sorted(glob.glob(os.path.expanduser("~/Library/Application Support/PlateMask/tracks/*.json")), key=os.path.getmtime)
if tracks:
    st = json.load(open(tracks[-1]))
    p0 = st["plates"][0]
    keys = p0.get("keys", [])
    from collections import Counter
    names = {0: "seed", 1: "confirmed", 2: "partial", 3: "occluded", 4: "out_of_frame", 5: "lost", 6: "manual", 7: "inferred"}
    print("sidecar:", tracks[-1], "detectHandled", st["detectHandled"], "trackHandled", st["trackHandled"], flush=True)
    print("plate1: seedFrame", p0.get("seedFrame"), "keys", len(keys), "states", {names[k]: v for k, v in Counter(k["s"] for k in keys).items()}, flush=True)
    for k in keys:
        if k["f"] in (0, 10, 50, 100, 118, 169):
            print("   f=%d %s q=%s" % (k["f"], names[k["s"]], [round(v) for v in k["q"]]), flush=True)
print("DONE", flush=True)
