#!/usr/bin/env python3
"""Hand-soldering check: copper gaps between tracks and other nets' pads, from model.json."""
import json, math, sys
m = json.load(open(sys.argv[1])); LIM = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
def pt_seg(p, a, b):
    (ax, ay), (bx, by), (px, py) = a, b, p; dx, dy = bx-ax, by-ay; L2 = dx*dx+dy*dy
    t = 0 if L2 == 0 else max(0, min(1, ((px-ax)*dx+(py-ay)*dy)/L2)); return math.hypot(px-ax-t*dx, py-ay-t*dy)
tight = []
for net, L, a, b, w in m["segs"]:
    for p in m["pads"]:
        if p["net"] == net: continue
        g = pt_seg((p["x"], p["y"]), a, b) - p["dia"]/2 - w/2
        if g < LIM: tight.append((round(g, 2), f'{p["ref"]}.{p["pin"]}', net))
tight = sorted(set(tight))
print(f"track-to-pad gaps under {LIM} mm: {len(tight)}")
for g, pad, net in tight: print(f"  {g:.2f} mm  {pad} vs track {net}")
