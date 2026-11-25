#!/usr/bin/env python3
"""Write Gerber RS-274X + Excellon from model.json, including silkscreen."""
import json, sys, os, math, zipfile, glob
from PIL import Image, ImageDraw, ImageFont
import matplotlib
OUT = sys.argv[1]; NAME = "wifi-disk-ii-revB"
m = json.load(open(OUT + "/model.json"))
pads, segs, vias, holes, BW, BH = m["pads"], m["segs"], m["vias"], m["holes"], m["BW"], m["BH"]
G = 1.27
P = {(p["ref"], p["pin"]): p for p in pads}
FONT = os.path.join(os.path.dirname(matplotlib.__file__), "mpl-data/fonts/ttf/DejaVuSans-Bold.ttf")

class Layer:
    def __init__(s): s.flash = []; s.lines = []            # (x,y,d) ; (x1,y1,x2,y2,w)
    def write(s, path, func):
        aps = {}
        for *_, d in s.flash: aps.setdefault(round(d, 3), None)
        for *_, w in s.lines: aps.setdefault(round(w, 3), None)
        o = ["%FSLAX46Y46*%", "%MOMM*%", f"%TF.FileFunction,{func}*%", "%LPD*%", "G01*"]
        for i, d in enumerate(sorted(aps)): aps[d] = 10 + i; o.append(f"%ADD{10+i}C,{d:.3f}*%")
        c = lambda v: str(int(round(v * 1e6)))
        fy = lambda y: BH - y
        cur = None
        for x, y, d in s.flash:
            a = aps[round(d, 3)]
            if a != cur: o.append(f"D{a}*"); cur = a
            o.append(f"X{c(x)}Y{c(fy(y))}D03*")
        for x1, y1, x2, y2, w in s.lines:
            a = aps[round(w, 3)]
            if a != cur: o.append(f"D{a}*"); cur = a
            o.append(f"X{c(x1)}Y{c(fy(y1))}D02*"); o.append(f"X{c(x2)}Y{c(fy(y2))}D01*")
        o.append("M02*"); open(path, "w").write("\n".join(o) + "\n")
    def rect(s, x1, y1, x2, y2, w=0.15):
        for a, b in (((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))): s.lines.append((*a, *b, w))
    def circle(s, cx, cy, r, w=0.15, n=36):
        pts = [(cx + r*math.cos(2*math.pi*i/n), cy + r*math.sin(2*math.pi*i/n)) for i in range(n+1)]
        for a, b in zip(pts, pts[1:]): s.lines.append((*a, *b, w))
    def text(s, x, y, txt, h=1.1, anchor="l", rot=0):
        """filled text via scanlines; (x,y) = left/centre/right of baseline-centre line"""
        S = 25                                             # px per mm
        f = ImageFont.truetype(FONT, int(h / 0.73 * S))
        l, t, r, b = f.getbbox(txt); im = Image.new("L", (r - l + 4, b - t + 4), 0)
        ImageDraw.Draw(im).text((2 - l, 2 - t), txt, font=f, fill=255)
        if rot: im = im.rotate(rot, expand=True)
        w_mm, h_mm = im.width / S, im.height / S
        ox = x - (0 if anchor == "l" else w_mm/2 if anchor == "c" else w_mm); oy = y - h_mm/2
        px = im.load(); step = 3
        for j in range(0, im.height, step):
            i = 0
            while i < im.width:
                if px[i, j] > 110:
                    k = i
                    while k < im.width and px[k, j] > 110: k += 1
                    s.lines.append((ox + i/S + 0.06, oy + j/S, ox + (k-1)/S - 0.06 if k-1-i > 3 else ox + i/S + 0.06, oy + j/S, 0.13)); i = k
                else: i += 1
        return w_mm

cu = [Layer(), Layer()]; mask = [Layer(), Layer()]; silk = Layer(); edge = Layer()
for p in pads:
    for L in (0, 1): cu[L].flash.append((p["x"], p["y"], p["dia"])); mask[L].flash.append((p["x"], p["y"], p["dia"] + 0.1))
for net, x, y in vias:
    for L in (0, 1): cu[L].flash.append((x, y, 0.8))
for net, L, a, b, w in segs: cu[L].lines.append((*a, *b, w))
for x, y in holes:
    for L in (0, 1): mask[L].flash.append((x, y, 3.2 + 0.3))
edge.rect(0, 0, BW, BH, 0.1)

# ---------------- silkscreen ----------------
def xy(ref, pin): p = P[(ref, pin)]; return p["x"], p["y"]
T = silk.text
# horizontal DIPs: pin 1 at right end of back row, notch at right
for ref, n, val in (("U1", 20, "74LVC245"), ("U2", 14, "74x125")):
    x1, y1 = xy(ref, 1); xh, _ = xy(ref, n // 2); _, yf = xy(ref, n)
    L_, R_, T_, B_ = xh - 1.3, x1 + 1.3, y1 + 1.25, yf - 1.25; cy = (y1 + yf) / 2
    silk.rect(L_, T_, R_, B_)
    silk.lines += [(R_, cy - 0.9, R_ - 0.7, cy - 0.9, 0.15), (R_ - 0.7, cy - 0.9, R_ - 0.7, cy + 0.9, 0.15), (R_ - 0.7, cy + 0.9, R_, cy + 0.9, 0.15)]
    silk.flash.append((x1, y1 + 1.9, 0.6)); T((L_ + R_) / 2, cy, f"{ref} {val}", 1.1, "c")
# Pico
x1, y1 = xy("U3", 1); x40, _ = xy("U3", 40); cx = (x1 + x40) / 2; top = y1 - 1.37
silk.rect(cx - 10.5, top, cx + 10.5, top + 51.0)
silk.rect(cx - 4, top, cx + 4, top + 5.5); T(cx, top + 2.8, "USB", 1.2, "c")
T(cx, top + 20, "U3", 1.4, "c"); T(cx, top + 23, "PICO 2 W", 1.3, "c"); T(cx, top + 26, "IN SOCKETS", 1.0, "c")
T(cx, top + 48.5, "ANTENNA END", 1.0, "c")
silk.flash.append((x1 + 1.5, y1, 0.6)); T(x1 + 2.3, y1, "1", 1.0)
for pin, lab in ((20, "20"), (21, "21"), (40, "40")):
    x, y = xy("U3", pin); T(x - 1.3 if pin > 20 else x + 1.3, y, lab, 0.9, "r" if pin > 20 else "l")
# J1 shrouded header along the back edge: key notch on the odd-pin (back edge) side, pin 1 at right
x1, y1 = xy("J1", 1); x19, _ = xy("J1", 19); _, y2 = xy("J1", 2); cx = (x1 + x19) / 2; cy = (y1 + y2) / 2
L_, R_, T_, B_ = cx - 16.5, cx + 16.5, cy - 4.45, cy + 4.45
silk.lines += [(L_, B_, R_, B_, 0.2), (L_, T_, L_, B_, 0.2), (R_, T_, R_, B_, 0.2),
               (L_, T_, cx - 2.25, T_, 0.2), (cx + 2.25, T_, R_, T_, 0.2),
               (cx - 2.25, T_, cx - 2.25, T_ + 0.9, 0.2), (cx - 2.25, T_ + 0.9, cx + 2.25, T_ + 0.9, 0.2), (cx + 2.25, T_ + 0.9, cx + 2.25, T_, 0.2)]
T(R_ + 0.7, y1, "1", 1.3); silk.lines.append((R_ + 0.1, y1, R_ + 0.5, y1, 0.3))
T(L_, T_ - 1.3, "J1 DISK II", 1.0); T(R_, T_ - 1.3, "+/-12V ON J1", 0.9, "r")
# J2 test header (horizontal) with rotated labels in front of it
xa, y = xy("J2", 11); xb, _ = xy("J2", 1); silk.rect(xa - 1.25, y - 1.2, xb + 1.25, y + 1.2)
for i, lab in enumerate(["PH0", "PH1", "PH2", "PH3", "WRQ", "+5V", "ENB", "RD", "WR", "WP", "GND"]):
    x, _ = xy("J2", i + 1); T(x, y + 2.9, lab, 0.75, "c", rot=90)
T(xa - 1.5, y - 1.9, "J2", 0.8, "r"); T(xa - 1.5, y - 0.5, "TEST", 0.75, "r")
# two-pin parts
def body(ref, val, hw=3.15, hh=1.2):
    (xa, ya_), (xb, yb_) = xy(ref, 1), xy(ref, 2); cx, cy = (xa + xb) / 2, (ya_ + yb_) / 2
    if abs(ya_ - yb_) < 0.01:
        silk.rect(cx - hw, cy - hh, cx + hw, cy + hh)
        silk.lines += [(min(xa, xb) + 1.0, cy, cx - hw, cy, 0.15), (cx + hw, cy, max(xa, xb) - 1.0, cy, 0.15)]
        T(cx, cy, f"{ref} {val}".strip(), 0.9, "c")
    else:
        silk.rect(cx - hh, cy - hw, cx + hh, cy + hw)
        silk.lines += [(cx, min(ya_, yb_) + 1.0, cx, cy - hw, 0.15), (cx, cy + hw, cx, max(ya_, yb_) - 1.0, 0.15)]
        T(cx, cy, f"{ref} {val}".strip(), 0.9, "c", rot=90)
    return cx, cy
for ref, val in (("R1", "470"), ("R2", "470"), ("R3", "10K"), ("R4", "10K"), ("R5", "1K")): body(ref, val)
cx, cy = body("D1", "D1", hw=2.6, hh=1.35) if False else body("D1", "", hw=2.6, hh=1.35); kx, _ = xy("D1", 2)
silk.lines.append((cx + 2.2, cy - 1.35, cx + 2.2, cy + 1.35, 0.35)); T(kx + 1.3, cy, "K", 0.9)
for ref in ("C1", "C2"):
    (xa, ya_), (xb, yb_) = xy(ref, 1), xy(ref, 2); cx, cy = (xa + xb) / 2, (ya_ + yb_) / 2
    silk.rect(cx - 1.5, cy - 1.25, cx + 1.5, cy + 1.25); T(cx, cy + 2.2, f"{ref} 0.1uF", 0.8, "c")
(xa, ya_), (xb, yb_) = xy("C3", 1), xy("C3", 2); cx, cy = (xa + xb) / 2, ya_
silk.circle(cx, cy, 2.75); T(xa - 1.0, cy - 3.4, "+", 1.2, "c"); T(cx, cy + 3.7, "C3 10uF", 0.85, "c")
(xa, ya_), (xb, yb_) = xy("D2", 1), xy("D2", 2); cx = (xa + xb) / 2
silk.circle(cx, ya_, 2.1); T(xa, ya_ - 3.0, "+", 1.0, "c"); T(cx, ya_ + 3.2, "D2 PWR", 0.9, "c")
for ref, lab, side in (("SW1", "UP", "l"), ("SW2", "DOWN", "l"), ("SW3", "ENTER", "b"), ("SW4", "RESTART", "t")):
    x, y = xy(ref, 1); cx, cy = x + 3.25, y + 2.25
    silk.rect(cx - 3.0, cy - 3.0, cx + 3.0, cy + 3.0); silk.circle(cx, cy, 1.7)
    if side == "t": T(cx, cy - 4.2, f"{ref} {lab}", 1.0, "c")
    elif side == "b": T(cx, cy + 4.3, lab, 1.1, "c")
    else: T(cx, cy - 4.1 if ref == "SW1" else cy + 4.2, lab, 1.1, "c")
# OLED module outline (27.3 x 27.8, header 2.0 mm below its top edge) and pin labels
xg, yj = xy("J3", 1); xs, _ = xy("J3", 4); cx = (xg + xs) / 2
silk.rect(cx - 13.65, yj - 2.0, cx + 13.65, yj + 25.8); silk.rect(cx - 5.1, yj - 1.25, cx + 5.1, yj + 1.25)
for i, lab in enumerate(["GND", "VCC", "SCL", "SDA"]): T(xg + i * 2.54, yj + 3.0, lab, 0.75, "c", rot=90)
T(cx, yj + 12.5, "J3 OLED 0.96in", 1.1, "c"); T(cx, yj + 15, "SSD1306 I2C", 1.0, "c"); T(cx, yj + 20.5, "FRONT", 0.9, "c")
silk.rect(cx - 10.9, yj + 5.6, cx + 10.9, yj + 9.6, 0.1) if False else None
# title
T(7.0, 65.3, "WIFI DISK II DRIVE EMULATOR   REV B  2026-11", 1.1)

# ---------------- write files ----------------
G_ = lambda ext: f"{OUT}/{NAME}.{ext}"
for f in glob.glob(f"{OUT}/{NAME}*"): os.remove(f)
cu[0].write(G_("GTL"), "Copper,L1,Top"); cu[1].write(G_("GBL"), "Copper,L2,Bot")
mask[0].write(G_("GTS"), "Soldermask,Top"); mask[1].write(G_("GBS"), "Soldermask,Bot")
silk.write(G_("GTO"), "Legend,Top"); edge.write(G_("GKO"), "Profile,NP")
def drill(path, items, plated):
    tools = sorted({round(d, 2) for _, _, d in items})
    o = ["M48", "METRIC,TZ", f"; {'PLATED' if plated else 'NON-PLATED'} holes"] + [f"T{i+1}C{d:.3f}" for i, d in enumerate(tools)] + ["%", "G90", "G05"]
    for i, d in enumerate(tools):
        o.append(f"T{i+1}")
        o += [f"X{x:.3f}Y{BH - y:.3f}" for x, y, dd in items if round(dd, 2) == d]
    o += ["T0", "M30"]; open(path, "w").write("\n".join(o) + "\n")
drill(f"{OUT}/{NAME}-PTH.DRL", [(p["x"], p["y"], p["drill"]) for p in pads] + [(x, y, 0.4) for _, x, y in vias], True)
drill(f"{OUT}/{NAME}-NPTH.DRL", [(x, y, 3.2) for x, y in holes], False)
with zipfile.ZipFile(f"{OUT}/{NAME}-gerbers.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(glob.glob(f"{OUT}/{NAME}*")):
        if not f.endswith(".zip"): z.write(f, os.path.basename(f))
print("wrote", sorted(os.path.basename(f) for f in glob.glob(f"{OUT}/{NAME}*")))
