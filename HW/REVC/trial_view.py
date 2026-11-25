#!/usr/bin/env python3
"""Draw a routed rev C model.json (copper, pads, vias, OLED keepout): trial-route review."""
import json, sys
from PIL import Image, ImageDraw, ImageFont
m = json.load(open(sys.argv[1])); out = sys.argv[2]; S, M = 18, 40
W, H = int(m["BW"] * S) + 2 * M, int(m["BH"] * S) + 2 * M + 60
im = Image.new("RGB", (W, H), "white"); d = ImageDraw.Draw(im, "RGBA")
f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 15)
P = lambda x, y: (M + x * S, M + y * S)
d.rectangle([P(0, 0), P(m["BW"], m["BH"])], fill=(20, 90, 45), outline="black", width=3)
ox0, oy0, ox1, oy1 = m["oled"]
d.rectangle([P(ox0, oy0), P(ox1, oy1)], fill=(120, 170, 255, 45), outline=(120, 170, 255), width=2)
for hx, hy in m["holes"]:
    r = 1.6 * S; cx, cy = P(hx, hy); d.ellipse([cx - r, cy - r, cx + r, cy + r], fill="black")
col = {0: (230, 160, 30, 230), 1: (60, 170, 255, 200)}
for L in (1, 0):
    for net, l, a, b, w in m["segs"]:
        if l == L: d.line([P(*a), P(*b)], fill=col[L], width=max(2, int(w * S)))
for net, x, y in m["vias"]:
    r = 0.4 * S; cx, cy = P(x, y); d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(240, 240, 240), outline="black")
for p in m["pads"]:
    r = p["dia"] / 2 * S; cx, cy = P(p["x"], p["y"]); box = [cx - r, cy - r, cx + r, cy + r]
    (d.rectangle if p.get("square") else d.ellipse)(box, fill=(215, 185, 110), outline="black")
    if p["pin"] == 1: d.text((cx, cy - r - 9), p["ref"], fill="white", font=f, anchor="mm")
top = sum(((a[0]-b[0])**2+(a[1]-b[1])**2)**.5 for _, l, a, b, _ in m["segs"] if l == 0)
bot = sum(((a[0]-b[0])**2+(a[1]-b[1])**2)**.5 for _, l, a, b, _ in m["segs"] if l == 1)
d.text((M, H - 45), f"Rev C TRIAL ROUTE (not final). Orange = top copper, blue = bottom copper, white dots = vias "
       f"({len(m['vias'])}). Track: top {top:.0f} mm, bottom {bot:.0f} mm. Blue box = OLED.", fill="black", font=f)
im.save(out)
