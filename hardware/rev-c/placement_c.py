#!/usr/bin/env python3
"""Rev C placement proposal: parts only, no routing (Barry: agree placement first).

Draws the proposed rev C placement (rev B grid: 1.27 mm, board 99.06 x 68.58 mm)
to placement-revC.png, with the large OLED's footprint as a keepout, and checks
pad-to-pad clearance and part outlines against the keepout.

Run: python3 placement_c.py [out.png]
"""
import math, sys
from PIL import Image, ImageDraw, ImageFont

G = 1.27
BW, BH = 78 * G, 54 * G                 # 99.06 x 68.58 mm, same as rev B
HOLES = [(3*G, 3*G), (75*G, 3*G), (3*G, 51*G), (75*G, 51*G)]; HOLE_D = 3.2
CLR = 0.2                                # pad-to-pad clearance target (rev B DRC)

# (6) OLED 1.54in module outline: 44.45 W x 38.1 H. Lower-left corner
# tangent to the bottom-left mounting hole: left edge = hole x + r, bottom
# edge = hole y - r.
# Lowered 3.41 mm (Barry: up to 4 mm OK) so J3 sits on the grid at y 24.
_hx, _hy = HOLES[2]
OLED = (_hx + HOLE_D/2, 24*G - 2.0, _hx + HOLE_D/2 + 44.45, 24*G - 2.0 + 38.1)

pads = []        # (ref, pin, x, y, dia, square)
outlines = []    # (ref, kind, data, label)   kind: rect (x0,y0,x1,y1) | circle (cx,cy,r)
moved = set()

def pad(ref, pin, gx, gy, dia=1.7, square=False):
    pads.append((ref, pin, gx * G, gy * G, dia, square))

def rect(ref, x0, y0, x1, y1, label=None):
    outlines.append((ref, "rect", (x0, y0, x1, y1), label))

def circle(ref, cx, cy, r, label=None):
    outlines.append((ref, "circle", (cx, cy, r), label))

# ---------------------------------------------------------------- parts
# J1 Disk II 2x10 shrouded header: unchanged from rev B
for r in range(10):
    pad("J1", 2*r+1, 28 - 2*r, 5); pad("J1", 2*r+2, 28 - 2*r, 7)
rect("J1", 24.13 - 16.5, 7.62 - 4.45, 24.13 + 16.5, 7.62 + 4.45, "J1 DISK II")

# (1) J2 test header removed.

def dip(ref, xr, y, n, label):
    h = n // 2
    for i in range(h): pad(ref, i + 1, xr - 2 * i, y)
    for i in range(h): pad(ref, h + 1 + i, xr - 2 * (h - 1) + 2 * i, y + 6)
    x1, xh = xr * G, (xr - 2 * (h - 1)) * G
    rect(ref, xh - 1.3, y * G + 1.25, x1 + 1.3, (y + 6) * G - 1.25, f"{ref} {label}")

# (4)(5) U1, U2 up toward J1 (rows y=11/17, were 15/21); spread horizontally
# with each decoupling cap standing at its notch (pin 1 / Vcc) end.
UY = 12                                   # top pin row (bottom row = UY + 6)
# Row, left to right: U1, C1, R3, R4, U2, C2, then the Pico; 3 grid (3.81 mm)
# between neighbours (Barry: caps were too tight against the TTL). U1 and
# C1 slid toward the left edge (Barry) to make room for R3/R4 lying flat.
dip("U1", 23, UY, 20, "74LVC245"); moved.add("U1")
dip("U2", 52, UY, 14, "74x125");   moved.add("U2")

def two(ref, a, b, label, dia=1.6, square=False):
    pad(ref, 1, a[0], a[1], dia, square); pad(ref, 2, b[0], b[1], dia, square)

def body2(ref, label, hw=3.15, hh=1.2):
    (_, _, xa, ya, _, _), (_, _, xb, yb, _, _) = [p for p in pads if p[0] == ref]
    cx, cy = (xa + xb) / 2, (ya + yb) / 2
    if abs(ya - yb) < 0.01: rect(ref, cx - hw, cy - hh, cx + hw, cy + hh, label)
    else:                   rect(ref, cx - hh, cy - hw, cx + hh, cy + hw, label)

two("C1", (26, UY + 1), (26, UY + 5), "C1"); moved.add("C1")
rect("C1", 26*G - 1.25, (UY + 3)*G - 1.5, 26*G + 1.25, (UY + 3)*G + 1.5, "C1 0.1uF")
two("C2", (55, UY + 1), (55, UY + 5), "C2"); moved.add("C2")
rect("C2", 55*G - 1.25, (UY + 3)*G - 1.5, 55*G + 1.25, (UY + 3)*G + 1.5, "C2 0.1uF")

# (3) C3 (tall) tucked against the Pico's left pin column, sheltered by the
# socketed Pico (Barry); D1 horizontal above it, its pads in line with Pico
# pin 1 (Barry), a short run from its cathode to Pico pin 39 (VSYS).
two("D1", (46, 4), (54, 4), "D1", 1.7); body2("D1", "D1", hw=2.6, hh=1.35); moved.add("D1")
C3X = 58 * G - 0.85 - 0.3 - 2.75 - 2.0    # 2 mm further from the Pico (Barry)
C3Y = 8 * G
pads.append(("C3", 1, C3X - G, C3Y, 1.6, False)); pads.append(("C3", 2, C3X + G, C3Y, 1.6, False))
moved.add("C3"); circle("C3", C3X, C3Y, 2.75, None)
rect("C3L", C3X - 0.1, C3Y + 3.3, C3X + 0.1, C3Y + 3.5, "C3 10uF")

# (4) R3, R4 between U1 and U2 (Barry), horizontal, centred between the
# DIPs' pin rows. Their three nets meet here: +3V3 (U1 pins 1/20, C1), ENABLE* (on
# to U2), WRREQ* (J1 -> U1).
# Opened to 5.08 mm (rows UY+1, UY+5, on grid): 3 routing lanes between them
# per layer, the same as past C1/C2, so the channel is 3 lanes end to end.
for ref, dy in (("R3", -2.0), ("R4", 2.0)):
    y = (UY + 3 + dy) * G
    pads.append((ref, 1, 29 * G, y, 1.6, False)); pads.append((ref, 2, 37 * G, y, 1.6, False))
body2("R3", "R3 10K"); body2("R4", "R4 10K"); moved.update(("R3", "R4"))

# (5) R1, R2 just below U2, away from the buttons
two("R1", (44, UY + 9), (52, UY + 9), "R1"); body2("R1", "R1 470"); moved.add("R1")
two("R2", (44, UY + 12), (52, UY + 12), "R2"); body2("R2", "R2 470"); moved.add("R2")

# U3 Pico 2 W unchanged
for p in range(1, 21):  pad("U3", p, 58, 4 + 2*(p-1))
for p in range(21, 41): pad("U3", p, 72, 4 + 38 - 2*(p-21))
rect("U3", 65*G - 10.5, 4*G - 1.37, 65*G + 10.5, 4*G - 1.37 + 51.0, "U3 PICO 2 W")

# (6) J3: header 2.2 mm left of centred (Barry, fit check), x snapped to the grid.
for i in range(4): pad("J3", i + 1, 17 + 2*i, 24)
rect("J3", *OLED, "J3 OLED 1.54in (keepout)"); moved.add("J3")

# (7) buttons: UP/DOWN left edge 0.15in (3.81 mm) right of the OLED, cluster 0.3in lower
def sw(ref, x0, y0, label):            # x0, y0 in mm (pad 1)
    pads.append((ref, 1, x0, y0, 1.7, False)); pads.append((ref, 2, x0 + 6.5, y0, 1.7, False))
    pads.append((ref, 3, x0, y0 + 4.5, 1.7, False)); pads.append((ref, 4, x0 + 6.5, y0 + 4.5, 1.7, False))
    cx, cy = x0 + 3.25, y0 + 2.25
    rect(ref, cx - 3.0, cy - 3.0, cx + 3.0, cy + 3.0, label); moved.add(ref)
# Body left edge = pad-1 x + 0.25 (6 mm body centred on the 6.5 x 4.5 mm pads).
# Pad 1 is therefore off the 1.27 grid (short stubs, as for other off-grid pads).
SWX = OLED[2] + 2.0 - 0.25                # UP/DOWN body 2 mm right of the OLED (Barry)
sw("SW1", SWX, 35 * G, "UP"); sw("SW2", SWX, 43 * G, "DOWN"); sw("SW3", SWX + 8 * G, 39 * G, "ENTER")
# (12)(14) flipped again (Barry): SW4, then R5 (vertical), then D2 rotated
# 90 deg (pads one above the other), on one horizontal centre line, pushed
# right as far as the bottom-right mounting hole allows, 1.5 mm between
# bodies. D2 lines up under Pico pin 21's column side.
CL = 48 * G                               # common centre line
GAP = 1.5
DX = 73 * G                               # D2 centre x (hole clearance limits it)
DY = CL - 2.0                             # D2 nudged 2 mm up toward the Pico, away from the hole (Barry)
pads.append(("D2", 1, DX, DY - G, 1.8, True)); pads.append(("D2", 2, DX, DY + G, 1.8, True))
moved.add("D2"); circle("D2", DX, DY, 2.1, "D2 ACTIVITY")
RX = DX - 2.1 - GAP - 1.2                 # R5 centre x (body half-width 1.2)
pads.append(("R5", 1, RX, CL - 4 * G, 1.6, False)); pads.append(("R5", 2, RX, CL + 4 * G, 1.6, False))
body2("R5", "R5 180"); moved.add("R5")
SW4R = RX - 1.2 - GAP                     # SW4 body right edge
sw("SW4", SW4R - 6.0 - 0.25, CL - 2.25, "SW4 RESTART")

TITLE = "APPLE II WIFI FLOPPY EMULATOR  REV C  2026-11  BY BARRY HILLS"

# ---------------------------------------------------------------- checks
def check():
    msgs = []
    for i, a in enumerate(pads):
        for b in pads[i+1:]:
            if a[0] == b[0] and a[0] in ("SW1", "SW2", "SW3", "SW4"):
                continue
            d = math.hypot(a[2] - b[2], a[3] - b[3]) - a[4] / 2 - b[4] / 2
            if d < CLR:
                msgs.append(f"pads {a[0]}.{a[1]} / {b[0]}.{b[1]}: clearance {d:.2f} mm")
    ox0, oy0, ox1, oy1 = OLED
    for ref, kind, data, _ in outlines:
        if ref in ("J3", "U3"):
            continue
        if kind == "rect":
            x0, y0, x1, y1 = data
        else:
            cx, cy, r = data; x0, y0, x1, y1 = cx - r, cy - r, cx + r, cy + r
        if x0 < ox1 and x1 > ox0 and y0 < oy1 and y1 > oy0:
            msgs.append(f"{ref} outline overlaps the OLED keepout")
    for ref, pin, x, y, dia, _ in pads:
        if ref != "J3" and ox0 - dia/2 < x < ox1 + dia/2 and oy0 - dia/2 < y < oy1 + dia/2:
            msgs.append(f"pad {ref}.{pin} under the OLED")
        for hx, hy in HOLES:
            if math.hypot(x - hx, y - hy) < HOLE_D/2 + dia/2 + 0.5:
                msgs.append(f"pad {ref}.{pin} too near a mounting hole")
        if not (dia/2 + 0.5 <= x <= BW - dia/2 - 0.5 and dia/2 + 0.5 <= y <= BH - dia/2 - 0.5):
            msgs.append(f"pad {ref}.{pin} too near the board edge")
    return msgs

# ---------------------------------------------------------------- drawing
def draw(path, S=18, M=40):
    W, H = int(BW * S) + 2 * M, int(BH * S) + 2 * M + 150
    im = Image.new("RGB", (W, H), "white"); d = ImageDraw.Draw(im, "RGBA")
    try:
        f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 15)
        fs = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 13)
        fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20)
    except OSError:
        f = fs = fb = ImageFont.load_default()
    P = lambda x, y: (M + x * S, M + y * S)
    d.rectangle([P(0, 0), P(BW, BH)], fill=(20, 90, 45), outline="black", width=3)
    for hx, hy in HOLES:
        r = HOLE_D / 2 * S; cx, cy = P(hx, hy); d.ellipse([cx - r, cy - r, cx + r, cy + r], fill="black")
    ox0, oy0, ox1, oy1 = OLED
    d.rectangle([P(ox0, oy0), P(ox1, oy1)], fill=(120, 170, 255, 70), outline=(120, 170, 255), width=3)
    for ref, kind, data, label in outlines:
        col = (255, 170, 40) if ref in moved else (230, 230, 230)
        if kind == "rect":
            x0, y0, x1, y1 = data
            d.rectangle([P(x0, y0), P(x1, y1)], outline=col, width=2)
            tx, ty = P((x0 + x1) / 2, (y0 + y1) / 2)
        else:
            cx, cy, r = data
            d.ellipse([P(cx - r, cy - r), P(cx + r, cy + r)], outline=col, width=2)
            tx, ty = P(cx, cy + r + 1.2)
        if label:
            d.text((tx, ty), label, fill=col, font=fs, anchor="mm")
    for ref, pin, x, y, dia, sq in pads:
        r = dia / 2 * S; cx, cy = P(x, y)
        box = [cx - r, cy - r, cx + r, cy + r]
        (d.rectangle if sq else d.ellipse)(box, fill=(220, 180, 90), outline="black")
    # Pico pin numbers that items 12/14 refer to
    for pin, gx, gy in ((20, 58, 42), (21, 72, 42), (1, 58, 4)):
        cx, cy = P(gx * G, gy * G); d.text((cx, cy - 20), str(pin), fill="white", font=fs, anchor="mm")
    # dimensions
    def hdim(x0, x1, y, text):
        a, b = P(x0, y), P(x1, y)
        d.line([a, b], fill="yellow", width=2)
        for p in (a, b): d.line([(p[0], p[1] - 6), (p[0], p[1] + 6)], fill="yellow", width=2)
        d.text(((a[0] + b[0]) / 2, a[1] - 12), text, fill="yellow", font=fs, anchor="mm")
    def vdim(x, y0, y1, text):
        a, b = P(x, y0), P(x, y1)
        d.line([a, b], fill="yellow", width=2)
        for p in (a, b): d.line([(p[0] - 6, p[1]), (p[0] + 6, p[1])], fill="yellow", width=2)
        d.text((a[0] + 8, (a[1] + b[1]) / 2), text, fill="yellow", font=fs, anchor="lm")
    hdim(ox0, ox1, oy1 - 3, f"{ox1 - ox0:.2f} mm (1.75in)")
    vdim(ox0 + 2, oy0, oy1, f"{oy1 - oy0:.1f} mm (1.5in)")
    sw1_left = SWX + 0.25
    hdim(ox1, sw1_left, 35 * G - 2.2, f"{sw1_left - ox1:.2f} mm")
    # OLED corner tangent to the hole
    hx, hy = HOLES[2]; r = (HOLE_D / 2) * S; cx, cy = P(hx, hy)
    d.ellipse([cx - r - 3, cy - r - 3, cx + r + 3, cy + r + 3], outline="yellow", width=2)
    d.text(P(7.0, 65.3), TITLE, fill="white", font=f, anchor="lm")
    # legend
    y = M + BH * S + 20
    d.text((M, y), "Rev C placement proposal (no routing). Orange = moved or new, grey = as rev B, blue = OLED keepout.",
           fill="black", font=f)
    d.text((M, y + 24), "Grid 1.27 mm. J2 test header removed. OLED header position ASSUMED: centred, 2.0 mm below the module's top edge.",
           fill="black", font=f)
    msgs = check()
    d.text((M, y + 54), "Checks: " + ("pads >= 0.2 mm apart, nothing under the OLED, holes and edges clear: OK"
                                       if not msgs else f"{len(msgs)} problem(s): " + "; ".join(msgs[:4])),
           fill=(0, 120, 0) if not msgs else (200, 0, 0), font=fb)
    im.save(path)
    return msgs

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "placement-revC.png"
    msgs = draw(out)
    print(out, "|", "OK" if not msgs else "\n".join(msgs))
