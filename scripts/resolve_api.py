"""Helpers for driving DaVinci Resolve's scripting API (Resolve must be running with
Preferences > System > General > External scripting using = Local)."""
import os, sys, time

MOD = "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/Modules/"
if MOD not in sys.path:
    sys.path.append(MOD)
os.environ.setdefault("RESOLVE_SCRIPT_API", "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting")
os.environ.setdefault("RESOLVE_SCRIPT_LIB", "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fusionscript.so")
import DaVinciResolveScript as dvr  # noqa: E402


def connect(timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = dvr.scriptapp("Resolve")
        if r:
            return r
        time.sleep(3)
    raise SystemExit("Cannot connect to Resolve. Is it running with external scripting set to Local?")


def open_project(resolve, name):
    pm = resolve.GetProjectManager()
    proj = pm.LoadProject(name)
    if not proj:
        proj = pm.CreateProject(name)
    if not proj:
        raise SystemExit("cannot create/open project %s" % name)
    return pm, proj


def wait_render(proj, job_id, timeout=600):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = proj.GetRenderJobStatus(job_id)
        if st and st.get("JobStatus") in ("Complete", "Failed", "Cancelled"):
            return st
        time.sleep(2)
    return {"JobStatus": "Timeout"}
