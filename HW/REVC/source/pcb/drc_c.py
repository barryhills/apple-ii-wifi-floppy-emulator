#!/usr/bin/env python3
"""Independent geometric DRC + connectivity + schematic cross-check on model.json."""
import json, math, sys, re, itertools
m = json.load(open(sys.argv[1] + "/model.json")); SCH = sys.argv[2]
pads, segs, vias, holes, BW, BH = m["pads"], m["segs"], m["vias"], m["holes"], m["BW"], m["BH"]
MINCLR, EDGE, HOLE2HOLE = 0.2, 0.5, 0.5

def pt_seg(p, a, b):
    ax, ay = a; bx, by = b; px, py = p; dx, dy = bx-ax, by-ay
    L2 = dx*dx + dy*dy
    t = 0 if L2 == 0 else max(0, min(1, ((px-ax)*dx + (py-ay)*dy) / L2))
    return math.hypot(px - ax - t*dx, py - ay - t*dy)
def cross(a, b, c, d):
    def o(p, q, r): return (q[0]-p[0])*(r[1]-p[1]) - (q[1]-p[1])*(r[0]-p[0])
    return o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0
def seg_seg(a, b, c, d):
    if cross(a, b, c, d): return 0.0
    return min(pt_seg(a, c, d), pt_seg(b, c, d), pt_seg(c, a, b), pt_seg(d, a, b))

# items: (net, layers, p1, p2, radius, label)
items = []
for p in pads: items.append((p["net"], (0, 1), (p["x"], p["y"]), (p["x"], p["y"]), p["dia"]/2, f'{p["ref"]}.{p["pin"]}'))
for net, x, y in vias: items.append((net, (0, 1), (x, y), (x, y), 0.4, f"via@{x:.1f},{y:.1f}"))
for net, L, a, b, w in segs: items.append((net, (L,), tuple(a), tuple(b), w/2, f"trk{'TB'[L]}"))
errs = []; minclr = (9, None)
parent = list(range(len(items)))
def find(i):
    while parent[i] != i: parent[i] = parent[parent[i]]; i = parent[i]
    return i
for i, j in itertools.combinations(range(len(items)), 2):
    A, B = items[i], items[j]
    if not set(A[1]) & set(B[1]): continue
    if abs(A[2][0]-B[2][0]) > 80: continue
    d = seg_seg(A[2], A[3], B[2], B[3]) - A[4] - B[4]
    if A[0] == B[0]:
        if d < -0.05: parent[find(i)] = find(j)
    else:
        if d < minclr[0]: minclr = (d, (A[5], A[0], B[5], B[0], A[2], B[2]))
        if d < MINCLR - 1e-6: errs.append(f"CLEARANCE {d:.3f} {A[5]}({A[0]}) vs {B[5]}({B[0]}) near {A[2]}")
# connectivity
bynet = {}
for i, it in enumerate(items):
    if it[5].startswith(("trk", "via")): continue
    bynet.setdefault(it[0], []).append(i)
for net, idx in bynet.items():
    if net.startswith("NC"): continue
    roots = {find(i) for i in idx}
    if len(roots) > 1: errs.append(f"OPEN net {net}: {len(roots)} islands: " + ", ".join(items[i][5] for i in idx))
# dangling copper: every track/via must belong to a component containing a pad
padroots = {find(i) for idx in bynet.values() for i in idx}
for i, it in enumerate(items):
    if it[5].startswith(("trk", "via")) and find(i) not in padroots: errs.append(f"DANGLING {it[5]} {it[0]} {it[2]}")
# edge clearance
for it in items:
    for (x, y) in (it[2], it[3]):
        e = min(x, y, BW - x, BH - y) - it[4]
        if e < EDGE: errs.append(f"EDGE {e:.2f} {it[5]}")
# holes
drills = [(p["x"], p["y"], p["drill"]/2, f'{p["ref"]}.{p["pin"]}') for p in pads] + \
         [(x, y, 0.2, "via") for _, x, y in vias] + [(x, y, 1.6, "mount") for x, y in holes]
minh = 9
for a, b in itertools.combinations(drills, 2):
    d = math.hypot(a[0]-b[0], a[1]-b[1]) - a[2] - b[2]; minh = min(minh, d)
    if d < HOLE2HOLE: errs.append(f"HOLE2HOLE {d:.2f} {a[3]} {b[3]}")
for x, y in holes:                       # copper to mounting hole
    for it in items:
        d = seg_seg(it[2], it[3], (x, y), (x, y)) - it[4] - 1.6
        if d < 0.4: errs.append(f"MOUNT-HOLE {d:.2f} {it[5]}")
for p in pads:
    if (p["dia"] - p["drill"]) / 2 < 0.3: errs.append(f"ANNULAR {p['ref']}.{p['pin']}")

# ---- schematic cross-check: parse sch.py and compare (ref,pin)->net ----
src = open(SCH).read()
alias = {"+5V_A2": "+5V", "ENABLE_3V*": "ENABLE_3V", "WRREQ_3V*": "WRREQ_3V"}
al = lambda n: alias.get(n, n)
sch = {}
for blk in src.split("\nic(")[1:]:
    ref = re.search(r'"(J1|U1|U2|U3|J3)"', blk).group(1)
    body = blk.split("\nt(")[0]
    if ref == "J1": continue
    for num, net in re.findall(r'\((\d+),"[^"]*",("[^"]*"|None)\)', body):
        sch[(ref, int(num))] = None if net == "None" else al(net.strip('"'))
jl = re.search(r"J=\[(.*?)\]\n", src, re.S).group(1)
for num, net in re.findall(r'\((\d+),("[^"]*"|None)\)', jl): sch[("J1", int(num))] = None if net == "None" else al(net.strip('"'))
for ref, a, b in re.findall(r'two\(\d+,y,"(\w+)","[^"]*","([^"]*)","([^"]*)"', src):
    sch[(ref, "a")] = al(a); sch[(ref, "b")] = al(b)
brd = {(p["ref"], p["pin"]): p["net"] for p in pads}
mism = []
for (ref, pin), net in sch.items():
    if pin in ("a", "b"): continue
    b = brd.get((ref, pin))
    if net is None:
        if not b.startswith("NC"): mism.append(f"{ref}.{pin}: sch NC, board {b}")
    elif b != net: mism.append(f"{ref}.{pin}: sch {net}, board {b}")
twop = {"D1": 0, "C1": 0, "C2": 0, "C3": 0, "R1": 1, "R2": 1, "R3": 0, "R4": 0, "R5": 0, "D2": 0}   # 1 = board pin order reversed vs sheet
for ref, rev in twop.items():
    s = (sch[(ref, "a")], sch[(ref, "b")]); b = (brd[(ref, 1)], brd[(ref, 2)])
    if set(s) != set(b): mism.append(f"{ref}: sch {s}, board {b}")
for ref in ("SW1", "SW2", "SW3", "SW4"):
    s = {sch[(ref, "a")], sch[(ref, "b")]}; b = {brd[(ref, 1)], brd[(ref, 4)]}
    if s != b: mism.append(f"{ref}: sch {s}, board {b}")
# pins on the board that are connected but not on the sheet (intentional additions get listed)
extra = sorted(f"{r}.{p}={n}" for (r, p), n in brd.items() if not n.startswith("NC") and (r, p) not in sch
               and r in ("J1", "U1", "U2", "U3", "J3"))
print("schematic pins compared:", len([k for k in sch if k[1] not in ('a','b')]), "mismatches:", mism)
print("board-only connections on sheet parts:", extra)
print(f"min clearance {minclr[0]:.3f} mm  {minclr[1]}")
print(f"min hole-to-hole {minh:.2f} mm; items {len(items)}")
print("DRC errors:", len(errs)); [print("  ", e) for e in errs[:40]]
sys.exit(1 if errs or mism else 0)
