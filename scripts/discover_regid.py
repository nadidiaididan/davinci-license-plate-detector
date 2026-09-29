#!/usr/bin/env python3
"""Find the Fusion registry id of the PlateMask OFX tool in a running Resolve."""
import os, sys, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve_api import connect
signal.alarm(120)
resolve = connect()
fusion = resolve.Fusion()
print("fusion:", fusion, flush=True)
found = {}
for ct in [0, 1, 2, 3, 4, 8, 16]:
    try:
        regs = fusion.GetRegList(ct)
    except Exception as e:
        print("GetRegList(%d) error %s" % (ct, e), flush=True)
        continue
    if not regs:
        print("GetRegList(%d) -> empty" % ct, flush=True)
        continue
    n = 0
    for k, v in regs.items():
        n += 1
        try:
            a = v.GetAttrs()
            rid = a.get("REGS_ID"); name = a.get("REGS_Name")
        except Exception:
            rid = str(v); name = ""
        if rid and ("platemask" in str(rid).lower() or "plate" in str(name).lower()):
            found[rid] = name
    print("GetRegList(%d) -> %d entries" % (ct, n), flush=True)
    if found:
        break
print("FOUND", found, flush=True)
