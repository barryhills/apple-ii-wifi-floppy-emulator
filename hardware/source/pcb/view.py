#!/usr/bin/env python3
"""Render the board by PARSING the written Gerber/Excellon files (not the model)."""
import re, sys, glob
from PIL import Image, ImageDraw
OUT, S = sys.argv[1], float(sys.argv[3]) if len(sys.argv) > 3 else 14
def gerber(path):
    aps = {}; cur = None; pos = (0, 0); shapes = []
    for ln in open(path).read().replace("\n", "").split("*"):
        ln = ln.strip("%")
        mm = re.match(r"ADD(\d+)C,([\d.]+)", ln)
        if mm: aps[int(mm.group(1))] = float(mm.group(2)); continue
        mm = re.match(r"D(\d+)$", ln)
        if mm and int(mm.group(1)) >= 10: cur = aps[int(mm.group(1))]; continue
        mm = re.match(r"X(-?\d+)Y(-?\d+)D0([123])", ln)
        if mm:
            p = (int(mm.group(1)) / 1e6, int(mm.group(2)) / 1e6); op = mm.group(3)
            if op == "1": shapes.append(("L", pos, p, cur))
            if op == "3": shapes.append(("F", p, p, cur))
            pos = p
    return shapes
def excellon(path):
    tools = {}; cur = None; hits = []
    for ln in open(path):
        ln = ln.strip(); mm = re.match(r"T(\d+)C([\d.]+)", ln)
        if mm: tools[int(mm.group(1))] = float(mm.group(2)); continue
        mm = re.match(r"T(\d+)$", ln)
        if mm and int(mm.group(1)) in tools: cur = tools[int(mm.group(1))]; continue
        mm = re.match(r"X([\d.-]+)Y([\d.-]+)", ln)
        if mm: hits.append((float(mm.group(1)), float(mm.group(2)), cur))
    return hits
f = lambda ext: glob.glob(f"{OUT}/*{ext}")[0]
edge = gerber(f(".GKO")); xs = [p[0] for s in edge for p in s[1:3]]; ys = [p[1] for s in edge for p in s[1:3]]
W, H = max(xs), max(ys); M = 2
def render(side):
    im = Image.new("RGB", (int((W + 2*M) * S), int((H + 2*M) * S)), "#ffffff"); d = ImageDraw.Draw(im)
    def tx(p):
        x = p[0] if side == "top" else W - p[0]
        return ((x + M) * S, (H - p[1] + M) * S)
    def draw(shapes, col):
        for k, a, b, w in shapes:
            A, B = tx(a), tx(b); r = w * S / 2
            if k == "L": d.line([A, B], fill=col, width=max(1, int(round(w * S))))
            d.ellipse([A[0]-r, A[1]-r, A[0]+r, A[1]+r], fill=col); d.ellipse([B[0]-r, B[1]-r, B[0]+r, B[1]+r], fill=col)
    d.rectangle([tx((0, H)) if side == "top" else tx((W, H)), tx((W, 0)) if side == "top" else tx((0, 0))], fill="#0b5d2a")
    other = gerber(f(".GBL" if side == "top" else ".GTL")); draw([s for s in other if s[0] == "L"], "#0f7a38")
    draw(gerber(f(".GTL" if side == "top" else ".GBL")), "#d8a531")
    draw(gerber(f(".GTS" if side == "top" else ".GBS")), "#e9d9a0")
    if side == "top": draw(gerber(f(".GTO")), "#ffffff")
    for path in glob.glob(f"{OUT}/*.DRL"):
        for x, y, dia in excellon(path): draw([("F", (x, y), (x, y), dia)], "#111111")
    return im
render("top").save(sys.argv[2] + "-top.png"); render("bottom").save(sys.argv[2] + "-bottom.png")
print("board", W, "x", H, "mm")
