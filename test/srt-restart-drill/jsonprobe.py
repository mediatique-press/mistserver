#!/usr/bin/env python3
"""Read a Mist json_<stream>.js body on stdin, print: live trackcount codecs."""
import json
import sys

raw = sys.stdin.read()
try:
    j = json.loads(raw)
except Exception:
    print("- - -")
    sys.exit()
if "error" in j and "meta" not in j:
    print("err 0 " + str(j["error"]).replace(" ", "_")[:40])
    sys.exit()
m = j.get("meta", {}) or {}
tr = m.get("tracks", {}) or {}
codecs = ",".join(sorted(f"{v.get('codec', '?')}{v.get('height') or ''}" for v in tr.values())) or "-"
print(f"{m.get('live', '-')} {len(tr)} {codecs}")
