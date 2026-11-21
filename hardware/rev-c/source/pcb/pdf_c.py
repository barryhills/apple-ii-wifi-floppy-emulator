#!/usr/bin/env python3
"""Rev C 1:1 fit-check PDF (US Letter) from the Gerbers, drawn with PIL at 600 dpi.
Print at 100% / "actual size". From rev B's pdf_b.py (which used reportlab)."""
import sys, glob
from PIL import Image, ImageDraw, ImageFont
OUT = sys.argv[1]
src = open(__file__.replace("pdf_c.py", "view_c.py")).read().split("f = lambda ext")[0]; ns = {}
sys.argv = ["view_c.py", OUT, "/dev/null/x", "1"]; exec(src, ns)
g = lambda e: ns["gerber"](glob.glob(f"{OUT}/*{e}")[0])
DPI = 600; S = DPI / 25.4
W, H = int(8.5 * DPI), int(11 * DPI)
im = Image.new("L", (W, H), 255); d = ImageDraw.Draw(im)
f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 52)
fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 60)
ox, oy = 20 * S, 60 * S + 68.58 * S          # board origin (bottom-left) on the page; y up
P = lambda x, y: (ox + x * S, oy - y * S)
d.text((ox, 20 * S), "Apple II WiFi Floppy Emulator rev C - 1:1 fit check. Print at 100% / \"actual size\", NOT fit-to-page.", font=fb, fill=0)
d.text((ox, 27 * S), "Check: the bar below must measure 50.0 mm. Then lay each part on its outline; leads must land on the dots.", font=f, fill=0)
by = oy + 12 * S
d.line([(ox, by), (ox + 50 * S, by)], fill=0, width=8); d.line([(ox, by - 2 * S), (ox, by + 2 * S)], fill=0, width=8)
d.line([(ox + 50 * S, by - 2 * S), (ox + 50 * S, by + 2 * S)], fill=0, width=8); d.text((ox + 52 * S, by - 1.5 * S), "50 mm", font=f, fill=0)
def draw(shapes, gray):
    for k, a, b, w in shapes:
        A, B = P(*a), P(*b); r = w * S / 2
        if k == "L": d.line([A, B], fill=gray, width=max(1, int(round(w * S))))
        if k == "S": d.rectangle([A[0]-r, A[1]-r, A[0]+r, A[1]+r], fill=gray); continue
        d.ellipse([A[0]-r, A[1]-r, A[0]+r, A[1]+r], fill=gray); d.ellipse([B[0]-r, B[1]-r, B[0]+r, B[1]+r], fill=gray)
draw(g(".GKO"), 0); draw(g(".GTS"), 190); draw(g(".GTO"), 0)
for p in glob.glob(f"{OUT}/*.DRL"):
    for x, y, dia in ns["excellon"](p):
        A = P(x, y); r = dia / 2 * S
        d.ellipse([A[0]-r, A[1]-r, A[0]+r, A[1]+r], fill=255 if dia > 2 else 0, outline=0, width=3)
im.save(f"{OUT}/fit-check-revC-1to1.pdf", resolution=DPI)
print("wrote", f"{OUT}/fit-check-revC-1to1.pdf")
