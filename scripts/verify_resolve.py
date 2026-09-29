#!/usr/bin/env python3
"""End-to-end check inside a running DaVinci Resolve: import the synthetic clip, put the PlateMask
OpenFX node in the clip's Fusion composition, run detection + tracking through the plugin's request
parameters, and render the timeline. Every step is logged and guarded by a watchdog."""
import os, sys, json, time, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve_api import connect, wait_render

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIP = os.path.join(ROOT, "testdata", "synth_plate_24.mp4")
OUT = os.path.join(ROOT, "build", "resolve_render")
PROJECT = os.environ.get("PM_PROJECT", "PlateMaskVerify_" + time.strftime("%H%M%S"))
os.makedirs(OUT, exist_ok=True)


def step(name, fn, timeout=90):
    print("step:", name, flush=True)
    signal.alarm(timeout)
    r = fn()
    signal.alarm(0)
    print("   ->", r if isinstance(r, (bool, int, float, str, list, dict, type(None))) else type(r).__name__, flush=True)
    return r


resolve = step("connect", connect, timeout=200)
print("Resolve", resolve.GetVersionString(), "page", resolve.GetCurrentPage(), flush=True)
pm = resolve.GetProjectManager()
proj = step("create project", lambda: pm.CreateProject(PROJECT))
if not proj:
    raise SystemExit("cannot create project")
step("set fps 24", lambda: proj.SetSetting("timelineFrameRate", "24"))
step("set resolution", lambda: proj.SetSetting("timelineResolutionWidth", "960") and proj.SetSetting("timelineResolutionHeight", "540"))
mp = proj.GetMediaPool()
items = step("import media", lambda: mp.ImportMedia([CLIP]))
if not items:
    raise SystemExit("import failed")
tl = step("create timeline", lambda: mp.CreateTimelineFromClips("verify", items))
if not tl:
    raise SystemExit("no timeline")
ti = step("timeline item", lambda: tl.GetItemListInTrack("video", 1)[0])
print("   item", ti.GetName(), ti.GetStart(), ti.GetEnd(), ti.GetDuration(), flush=True)
step("open fusion page", lambda: resolve.OpenPage("fusion"))
comp = step("fusion comp", lambda: ti.GetFusionCompByIndex(1))
if not comp:
    raise SystemExit("no Fusion comp")
tools = step("tool list", lambda: comp.GetToolList(False) or {})
regs = {k: v.GetAttrs().get("TOOLS_RegID") for k, v in tools.items()}
print("   tools", regs, flush=True)
media_in = next((t for t in tools.values() if t.GetAttrs().get("TOOLS_RegID") == "MediaIn"), None)
media_out = next((t for t in tools.values() if t.GetAttrs().get("TOOLS_RegID") == "MediaOut"), None)
plate = None
for reg in ["ofx.ch.platemask.PlateMask", "ch.platemask.PlateMask", "OFX.ch.platemask.PlateMask", "ofx.ch_platemask_PlateMask"]:
    plate = step("add tool " + reg, lambda: comp.AddTool(reg))
    if plate:
        break
if not plate:
    raise SystemExit("PlateMask OFX tool not found in Fusion registry")
attrs = plate.GetAttrs()
print("   plate tool", attrs.get("TOOLS_RegID"), attrs.get("TOOLS_Name"), flush=True)
if media_in and media_out:
    step("connect source", lambda: plate.ConnectInput("Source", media_in))
    step("connect out", lambda: media_out.ConnectInput("Input", plate))
inputs = step("inputs", lambda: plate.GetInputList() or {})
print("   input ids", sorted(inputs.keys()), flush=True)
json.dump({"project": PROJECT, "regid": attrs.get("TOOLS_RegID"), "inputs": sorted(inputs.keys())},
          open(os.path.join(OUT, "state.json"), "w"))
print("DONE phase 1", flush=True)

# ---------------------------------------------------------------- phase 2: detect, track, render
def set_input(name, value, t=None):
    if name not in inputs:
        print("   (no input %s)" % name, flush=True)
        return None
    return plate.SetInput(name, value) if t is None else plate.SetInput(name, value, t)

# Seed on the plate at comp frame 10 (synthetic clip: plate centre ~ (565, 270) of 960x540).
seed_x, seed_y = 565.0 / 960.0, 270.0 / 540.0
step("seed (normalised point)", lambda: set_input("seedPoint", [seed_x, seed_y]))
print("   seed readback:", plate.GetInput("seedPoint"), flush=True)
step("requestPlate", lambda: set_input("requestPlate", 0))
step("requestFrame", lambda: set_input("requestFrame", 10))
step("detectRequest", lambda: set_input("detectRequest", 1))
step("trackRequest", lambda: set_input("trackRequest", 1))
step("blockSize", lambda: set_input("blockSize", 10))
step("expand", lambda: set_input("expand", 4))
step("back to edit page", lambda: resolve.OpenPage("deliver"))
settings = {"TargetDir": OUT, "CustomName": "platemask_verify", "SelectAllFrames": True, "ExportAudio": False}
step("render settings", lambda: proj.SetRenderSettings(settings))
job = step("add render job", lambda: proj.AddRenderJob())
step("start render", lambda: proj.StartRendering(job, isInteractiveMode=False))
status = step("wait render", lambda: wait_render(proj, job, 900), timeout=950)
print("   render status:", status, flush=True)
print("DONE phase 2", flush=True)
