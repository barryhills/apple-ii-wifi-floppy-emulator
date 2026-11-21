#!/usr/bin/env python3
"""Apple II WiFi Floppy Emulator - rev C PCB generator (model + router). From rev B's board_b.py."""
import heapq, math, os, sys, json, zipfile, itertools
G = 1.27
NX, NY = 79, 55                      # grid nodes
BW, BH = (NX - 1) * G, (NY - 1) * G  # 76.2 x 68.58 mm
TW = 0.4                             # trace width
VIA_D, VIA_H = 0.8, 0.4
CLR = 0.2                            # design clearance target
OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
os.makedirs(OUT, exist_ok=True)

pads = []   # dict(ref,pin,x,y(mm),net,drill,dia,grid=(ix,iy)|None)
nc = itertools.count()
def pad(ref, pin, gx, gy, net, drill=0.8, dia=1.6, mm=None):
    if net is None: net = f"NC{next(nc)}"
    if mm: x, y, grid = mm[0], mm[1], None
    else:  x, y, grid = gx * G, gy * G, (gx, gy)
    pads.append(dict(ref=ref, pin=pin, x=x, y=y, net=net, drill=drill, dia=dia, grid=grid))

# ---------------- netlist (schematic rev 0.4) + rev C placement ----------------
# Placement as agreed with Barry (hardware/rev-c/placement-revC.png, 2026-11-20).
# Changes from rev B: J2 test header removed; R5 fed from Pico pin 21 (GP16,
# net LED_ACT) instead of +3V3; 1.54in OLED (J3) with its keepout; parts moved.
def diph(ref, xr, y, nets):
    """DIP rotated 90 deg clockwise: pin 1 at the right end of the back row, notch at right"""
    n = len(nets); h = n // 2
    for i in range(h): pad(ref, i + 1, xr - 2 * i, y, nets[i])
    for i in range(h): pad(ref, h + 1 + i, xr - 2 * (h - 1) + 2 * i, y + 6, nets[h + i])
j1 = {1:"GND",2:"PH0",3:"GND",4:"PH1",5:"GND",6:"PH2",7:"GND",8:"PH3",9:None,10:"WRREQ*",
      11:"+5V",12:"+5V",13:None,14:"ENABLE*",15:None,16:"RDDATA",17:None,18:"WRDATA",19:None,20:"WRPROT"}
for r in range(10):                      # odd row at the back edge, pin 1 at the right
    pad("J1", 2*r+1, 28 - 2*r, 5, j1[2*r+1], 1.0, 1.7)
    pad("J1", 2*r+2, 28 - 2*r, 7, j1[2*r+2], 1.0, 1.7)
UY = 12
diph("U1", 23, UY, ["+3V3","PH0","PH1","PH2","PH3","ENABLE*","WRREQ*","WRDATA","GND","GND",
                    None,"WRDATA_3V","WRREQ_3V","ENABLE_3V","PH3_3V","PH2_3V","PH1_3V","PH0_3V","GND","+3V3"])
diph("U2", 52, UY, ["ENABLE*","RDDATA_R","RDDATA","ENABLE*","WRPROT_R","WRPROT","GND",
                    None,"GND","+5V",None,"GND","+5V","+5V"])
U3X, U3Y = 58, 4
pico = {3:"GND",4:"PH0_3V",5:"PH1_3V",6:"PH2_3V",7:"PH3_3V",8:"GND",9:"ENABLE_3V",10:"WRREQ_3V",
        11:"WRDATA_3V",12:"ENTER*",13:"GND",14:"RDDATA_3V",15:"WRPROT_3V",16:"SDA",17:"SCL",18:"GND",
        19:"UP*",20:"DOWN*",21:"LED_ACT",23:"GND",28:"GND",30:"RESTART*",33:"GND",36:"+3V3",38:"GND",39:"VSYS"}
for p in range(1, 21):  pad("U3", p, U3X, U3Y + 2*(p-1), pico.get(p), 1.0, 1.7)
for p in range(21, 41): pad("U3", p, U3X + 14, U3Y + 38 - 2*(p-21), pico.get(p), 1.0, 1.7)
# OLED 1.54in (44.45 x 38.1 outline): lower-left corner tangent to the
# bottom-left mounting hole. J3: Barry fit-checked the module on the 1:1 print
# (2026-11-20): its header sits 2.2 mm left of where a centred header would be;
# with that, the module's left edge lines up with the silkscreen outline. Pin
# centres 2.0 mm below the outline's top edge (confirmed). Pins snapped to the
# routing grid in x (x 17..23, a 0.035 mm shift); y stays off grid (stubs).
HOLES = [(3*G, 3*G), (75*G, 3*G), (3*G, 51*G), (75*G, 51*G)]; HOLE_D = 3.2
# Lowered (Barry, 2026-11-20: up to 4 mm is fine; the module's removable tabs
# may cover a sliver of the mounting hole) by 3.41 mm, so J3 sits on the grid
# (x 17..23, y 24) and clears U1's bottom row by 3 grid steps.
J3Y = 24
JY = J3Y * G
OLED = (HOLES[2][0] + HOLE_D/2, JY - 2.0, HOLES[2][0] + HOLE_D/2 + 44.45, JY - 2.0 + 38.1)
for i, n in enumerate(["GND", "+3V3", "SCL", "SDA"]): pad("J3", i+1, 17 + 2*i, J3Y, n, 1.0, 1.7)

def two(ref, a, b, na, nb, drill=0.8, dia=1.6):
    pad(ref, 1, a[0], a[1], na, drill, dia); pad(ref, 2, b[0], b[1], nb, drill, dia)
def two_mm(ref, a, b, na, nb, drill=0.8, dia=1.6):
    pad(ref, 1, 0, 0, na, drill, dia, mm=a); pad(ref, 2, 0, 0, nb, drill, dia, mm=b)
two("C1", (26, UY+1), (26, UY+5), "+3V3", "GND")
two("C2", (55, UY+1), (55, UY+5), "+5V", "GND")
two("R3", (37, UY+1), (29, UY+1), "ENABLE*", "+3V3")
two("R4", (37, UY+5), (29, UY+5), "WRREQ*", "+3V3")
two("R1", (44, UY+9), (52, UY+9), "RDDATA_R", "RDDATA_3V")
two("R2", (44, UY+12), (52, UY+12), "WRPROT_R", "WRPROT_3V")   # 3.81 mm below R1 (Barry: easier soldering)
two("D1", (46, 4), (54, 4), "+5V", "VSYS", 1.0, 1.7)        # 1=anode 2=cathode
C3X = 58*G - 0.85 - 0.3 - 2.75 - 2.0; C3Y = 8*G
two_mm("C3", (C3X - G, C3Y), (C3X + G, C3Y), "+5V", "GND")   # 1 = +
def sw(ref, x0, y0, net):
    pad(ref, 1, 0, 0, net, 1.0, 1.7, mm=(x0, y0))
    pad(ref, 2, 0, 0, None, 1.0, 1.7, mm=(x0 + 6.5, y0))
    pad(ref, 3, 0, 0, None, 1.0, 1.7, mm=(x0, y0 + 4.5))
    pad(ref, 4, 0, 0, "GND", 1.0, 1.7, mm=(x0 + 6.5, y0 + 4.5))
SWX = OLED[2] + 2.0 - 0.25                # UP/DOWN body 2 mm right of the OLED
sw("SW1", SWX, 35*G, "UP*"); sw("SW2", SWX, 43*G, "DOWN*"); sw("SW3", SWX + 8*G, 39*G, "ENTER*")
CL = 48*G; GAP = 1.5; DX = 73*G; DY = CL - 2.0
RX = DX - 2.1 - GAP - 1.2
two_mm("R5", (RX, CL - 4*G), (RX, CL + 4*G), "LED_ACT", "LED_A")
two_mm("D2", (DX, DY + G), (DX, DY - G), "LED_A", "GND", 0.8, 1.8)   # 1=anode (lower), square pads
for p in pads:
    if p["ref"] == "D2": p["square"] = True
sw("SW4", RX - 1.2 - GAP - 6.0 - 0.25, CL - 2.25, "RESTART*")

# ---------------- router ----------------
occ = [[[None]*NX for _ in range(NY)] for _ in range(2)]
padnode = {}                                # (ix,iy) -> net for on-grid TH pads
stubs = []                                  # (net, (x,y), (ix,iy)) bottom-layer stub from off-grid pad
for iy in range(NY):
    for ix in range(NX):
        if ix in (0, NX-1) or iy in (0, NY-1):
            occ[0][iy][ix] = occ[1][iy][ix] = "#"
        for hx, hy in HOLES:
            if math.hypot(ix*G - hx, iy*G - hy) < HOLE_D/2 + 0.9:
                occ[0][iy][ix] = occ[1][iy][ix] = "#"
for p in pads:
    if p["grid"]:
        ix, iy = p["grid"]; occ[0][iy][ix] = occ[1][iy][ix] = p["net"]; padnode[(ix, iy)] = p["net"]
for p in pads:
    if not p["grid"]:
        cx, cy = round(p["x"]/G), round(p["y"]/G)
        for iy in range(cy-2, cy+3):
            for ix in range(cx-2, cx+3):
                if math.hypot(ix*G - p["x"], iy*G - p["y"]) < p["dia"]/2 + TW/2 + CLR + 0.08 and (ix, iy) not in padnode:
                    occ[0][iy][ix] = occ[1][iy][ix] = "#"
for p in pads:
    if not p["grid"] and not p["net"].startswith("NC"):
        # stub node: nearest node straight below/right that is outside the blocked ring
        best = None
        for iy in range(round(p["y"]/G)-3, round(p["y"]/G)+4):
            for ix in range(round(p["x"]/G)-3, round(p["x"]/G)+4):
                if not (0 < ix < NX-1 and 0 < iy < NY-1) or (ix, iy) in padnode: continue
                d = math.hypot(ix*G - p["x"], iy*G - p["y"])
                # must not pass near the sibling NC pads of the same switch
                ok = all(q is p or q["grid"] or
                         seg_ok for q in pads for seg_ok in [True])
                if (best is None or d < best[0]) and d > 0.3: best = (d, ix, iy)
        _, ix, iy = best
        occ[1][iy][ix] = p["net"]; occ[0][iy][ix] = "#"
        stubs.append((p["net"], (p["x"], p["y"]), (ix, iy)))
        p["stub"] = (ix, iy)

def via_ok(ix, iy, net):
    if (ix, iy) in padnode: return False
    for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
        n = padnode.get((ix+dx, iy+dy))
        if n is not None and n != net: return False
    for p in pads:                       # keep vias away from off-grid pads
        if not p["grid"] and math.hypot(ix*G - p["x"], iy*G - p["y"]) < p["dia"]/2 + VIA_D/2 + CLR + 0.05: return False
    return True

# Hand soldering (Barry): a track one grid step (1.27 mm) from another net's
# pad leaves only ~0.22 mm of copper gap. Make such nodes expensive, so tracks
# keep two steps away (~1.5 mm gap) wherever there is room, and squeeze past a
# pad only when there is no other way (e.g. between two adjacent DIP pins).
NEAR_COST = float(os.environ.get("NEAR_COST", "6"))
near = [[set() for _ in range(NX)] for _ in range(NY)]   # nets of pads within one step
for p in pads:
    if p["net"].startswith("NC") and False: pass
    cx, cy = round(p["x"]/G), round(p["y"]/G)
    for iy in range(max(0, cy-2), min(NY, cy+3)):
        for ix in range(max(0, cx-2), min(NX, cx+3)):
            d = math.hypot(ix*G - p["x"], iy*G - p["y"])
            if 0.3 < d < p["dia"]/2 + TW/2 + 0.45: near[iy][ix].add(p["net"])

edges = {}   # net -> set of (layer, (ix,iy), (jx,jy))
vias = {}    # net -> set of (ix,iy)
def route_net(net, terms):
    """terms: list of sets of start states (layer,ix,iy)."""
    tree = set(terms[0]); E = edges.setdefault(net, set()); V = vias.setdefault(net, set())
    remaining = terms[1:]
    while remaining:
        # pick the remaining terminal closest to the tree
        def d(t): return min(abs(a[1]-b[1]) + abs(a[2]-b[2]) for a in t for b in tree)
        remaining.sort(key=d); t = remaining.pop(0)
        dist = {s: 0 for s in t}; prev = {}; pq = [(0, s) for s in t]; heapq.heapify(pq); goal = None
        while pq:
            c, s = heapq.heappop(pq)
            if c > dist.get(s, 1e9): continue
            if s in tree: goal = s; break
            L, ix, iy = s
            for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
                jx, jy = ix+dx, iy+dy
                o = occ[L][jy][jx]
                if o is not None and o != net: continue
                step = 1.0 + (0.4 if (L == 0) == (dx == 0) else 0.0)   # top prefers horizontal, bottom vertical
                if near[jy][jx] - {net}: step += NEAR_COST
                # discourage hugging other nets' pads
                ns = (L, jx, jy); nc_ = c + step
                if nc_ < dist.get(ns, 1e9): dist[ns] = nc_; prev[ns] = s; heapq.heappush(pq, (nc_, ns))
            o = occ[1-L][iy][ix]
            if o is None or o == net:
                free = padnode.get((ix, iy)) == net
                if free or via_ok(ix, iy, net):
                    ns = (1-L, ix, iy); nc_ = c + (0 if free else 12)
                    if nc_ < dist.get(ns, 1e9): dist[ns] = nc_; prev[ns] = s; heapq.heappush(pq, (nc_, ns))
        if goal is None: print("  FAIL", net, sorted(t)[:2], "reached", len(dist)); return False
        s = goal; path = [s]
        while s in prev: s = prev[s]; path.append(s)
        for a, b in zip(path, path[1:]):
            if a[0] != b[0]:
                if padnode.get((a[1], a[2])) != net: V.add((a[1], a[2]))
            else: E.add((a[0], (a[1], a[2]), (b[1], b[2])))
        for L, ix, iy in path:
            occ[L][iy][ix] = net; tree.add((L, ix, iy))
            if (ix, iy) in V: occ[1-L][iy][ix] = net; tree.add((1-L, ix, iy))
        tree |= set(t)
    return True

netpads = {}
for p in pads:
    if not p["net"].startswith("NC"): netpads.setdefault(p["net"], []).append(p)
def terms_of(net):
    T = []
    for p in netpads[net]:
        if p["grid"]: ix, iy = p["grid"]; T.append({(0, ix, iy), (1, ix, iy)})
        else: ix, iy = p["stub"]; T.append({(1, ix, iy)})
    return T
def span(net):
    xs = [p["x"] for p in netpads[net]]; ys = [p["y"] for p in netpads[net]]
    return (max(xs)-min(xs)) + (max(ys)-min(ys))
POWER = ["+5V", "VSYS", "+3V3", "GND"]
order = sorted([n for n in netpads if n not in POWER], key=span) + POWER
ORDER_OVERRIDE = os.environ.get("ORDER")
if ORDER_OVERRIDE: order = ORDER_OVERRIDE.split(",")
failed = [n for n in order if len(netpads[n]) > 1 and not route_net(n, terms_of(n))]
print("nets:", len(order), "failed:", failed)
if failed: sys.exit(1)

# ---------------- copper geometry ----------------
def merge(E):
    """merge unit edges into maximal straight segments -> list of (layer,(x1,y1),(x2,y2)) in mm"""
    out = []
    for L in (0, 1):
        H = {}; Vv = {}
        for l, a, b in E:
            if l != L: continue
            a, b = sorted((a, b))
            if a[1] == b[1]: H.setdefault(a[1], set()).add(a[0])
            else: Vv.setdefault(a[0], set()).add(a[1])
        for y, xs in H.items():
            xs = sorted(xs); s = xs[0]; e = s
            for x in xs[1:] + [None]:
                if x is not None and x == e + 1: e = x; continue
                out.append((L, (s*G, y*G), ((e+1)*G, y*G)))
                if x is not None: s = e = x
        for x, ys in Vv.items():
            ys = sorted(ys); s = ys[0]; e = s
            for y in ys[1:] + [None]:
                if y is not None and y == e + 1: e = y; continue
                out.append((L, (x*G, s*G), (x*G, (e+1)*G)))
                if y is not None: s = e = y
    return out
segs = []   # (net, layer, p1, p2, width)
for net, E in edges.items():
    for L, a, b in merge(E): segs.append((net, L, a, b, TW))
for net, pxy, (ix, iy) in stubs: segs.append((net, 1, pxy, (ix*G, iy*G), TW))
vialist = [(net, ix*G, iy*G) for net, V in vias.items() for ix, iy in V]
json.dump(dict(pads=pads, segs=segs, vias=vialist, holes=HOLES, BW=BW, BH=BH, oled=OLED), open(f"{OUT}/model.json", "w"))
print("segments:", len(segs), "vias:", len(vialist),
      "track mm top/bot:", *[round(sum(math.dist(s[2], s[3]) for s in segs if s[1] == L)) for L in (0, 1)])
