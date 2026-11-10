#!/usr/bin/env python3
# Draws res/Tapejeans.svg. VCV Rack can't draw SVG <text>, so every label is
# turned into outline paths with fontTools. Positions are in mm and must match
# src/Tapejeans.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import math
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 182.88, 128.5  # 36 HP

DENIM = "#2C4766"
DENIM_DARK = "#22384F"
INK = "#F1EBDD"
SOFT = "#A9B6C4"
STITCH = "#E8A03A"
OUTBOX = "#16202C"
GENE = "#F2D050"

fonts = {}


def text(s, x, y, size, font=FONT_MED, fill=INK, anchor="middle", spacing=0.04):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def stitch(x1, y1, x2, y2, w=0.3):
    add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{STITCH}" stroke-width="{w}" '
        f'stroke-dasharray="1.2 0.8" stroke-linecap="round"/>')


def tick(cx, cy, value, label, r0=7.0, r1=8.1, fill=INK, size=2.4):
    """A mark at a knob position (value 0..1 over Rack's -150..+150 degree sweep)."""
    a = math.radians(-150 + 300 * value)
    sx, sy = math.sin(a), -math.cos(a)
    add(f'<line x1="{cx + r0 * sx:.3f}" y1="{cy + r0 * sy:.3f}" x2="{cx + r1 * sx:.3f}" '
        f'y2="{cy + r1 * sy:.3f}" stroke="{fill}" stroke-width="0.35"/>')
    if label:
        rl = r1 + 1.6
        add(text(label, cx + rl * sx, cy + rl * sy + size * 0.36, size, FONT_BOLD, fill))


add(f'<rect width="{W}" height="{H}" fill="{DENIM}"/>')
# a faint twill texture
# (lines are clipped to the panel by hand: Rack's SVG renderer has no clipPath)
for i in range(-130, 160, 3):
    x1, y1, x2, y2 = float(i), 0.0, i + H, H
    if x1 < 0:
        y1, x1 = -x1, 0.0
    if x2 > W:
        y2, x2 = H - (x2 - W), W
    if x2 > x1:
        add(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="{DENIM_DARK}" stroke-width="0.6" opacity="0.35"/>')

# stitching
stitch(2.0, 6.2, W - 2.0, 6.2)
stitch(2.0, H - 2.2, W - 2.0, H - 2.2)
stitch(2.0, 50.4, W - 2.0, 50.4)
stitch(2.0, 93.4, W - 2.0, 93.4)

# title
add(text("TAPEJEANS", 22.0, 10.7, 4.0, FONT_BOLD, INK, anchor="start", spacing=0.1))
add(text("reel  /  splice  /  gene", W - 22.0, 10.4, 2.8, FONT_MED, SOFT, anchor="end"))

# display window
add(f'<rect x="4.4" y="11.9" width="{W - 8.8:.2f}" height="37.2" rx="1.6" fill="#0E0E12"/>')

# knob row
colX = [16.0, 46.0, 76.0, 106.0, 136.0, 166.0]
knobY, attY, cvY = 62.0, 79.2, 88.0
names = ["S.O.S.", "GENE SIZE", "VARI-SPEED", "MORPH", "SLIDE", "ORGANIZE"]
hints = [("input", "reel"), ("splice", "tiny"), ("rev", "fwd"), ("gaps", "layers"), ("start", "end"), ("first", "last")]
for x, n, (lo, hi) in zip(colX, names, hints):
    add(text(n, x, 54.4, 3.2, FONT_BOLD, INK, spacing=0.05))
    add(text(lo, x - 9.0, 71.0, 2.4, FONT_MED, SOFT))
    add(text(hi, x + 9.0, 71.0, 2.4, FONT_MED, SOFT))
    add(text("CV", x - 7.4, cvY + 1.0, 2.6, FONT_BOLD, SOFT))
for i in (1, 2, 4):
    add(text("amt", colX[i] - 7.4, attY + 0.9, 2.4, FONT_MED, SOFT))

# knob landmarks: Vari-Speed x1 either side, stop at noon; Morph 1/1 and pitch layers
tick(colX[2], knobY, 0.25, "x1")
tick(colX[2], knobY, 0.75, "x1")
tick(colX[2], knobY, 0.5, "", fill=STITCH)
tick(colX[3], knobY, 0.2, "1/1")
tick(colX[3], knobY, 0.85, "", fill=GENE)
tick(colX[3], knobY, 0.5, "")
tick(colX[0], knobY, 1.0, "")
tick(colX[1], knobY, 0.0, "")
add(text("CCW = whole splice", colX[1], 74.4, 2.2, FONT_MED, SOFT))
add(text("noon = stopped", colX[2], 74.4, 2.2, FONT_MED, SOFT))
add(text("past 1/1 = overlap", colX[3], 74.4, 2.2, FONT_MED, SOFT))
add(text("moves the gene", colX[4], 74.4, 2.2, FONT_MED, SOFT))
add(text("picks next splice", colX[5], 74.4, 2.2, FONT_MED, SOFT))
add(text("what you hear", colX[0], 74.4, 2.2, FONT_MED, SOFT))

# buttons
btnY = 102.4
labels = ["REC", "REC NEW", "SPLICE", "NEXT", "DEL MARK", "DEL AUDIO", "CLR MARKS", "CLR REEL", "GAIN"]
# one-line hints; the hardware ("hw:") mappings moved to the tooltips and the manual
hint = ["into this splice", "new splice", "add marker", "next splice", "join with next", "erase splice",
        "hold 1 s", "hold 1 s", "input level"]
for i, (lab, h) in enumerate(zip(labels, hint)):
    x = 12.0 + 19.6 * i if i < 8 else 170.0
    add(text(lab, x, 97.4, 2.9, FONT_BOLD, INK if i < 6 or i == 8 else "#F2A0A0", spacing=0.04))
    add(text(h, x, 109.6, 2.2, FONT_MED, SOFT))

# ---- jacks ----
jackY = 120.6
add(f'<rect x="101.0" y="111.0" width="{W - 105.0:.2f}" height="15.8" rx="1.2" fill="{OUTBOX}"/>')
for i, lab in enumerate(["IN L", "IN R", "CLK", "PLAY", "REC", "SPLICE", "SHIFT"]):
    add(text(lab, 14.0 + 13.0 * i, 114.6, 2.9, FONT_BOLD, INK))
for i, lab in enumerate(["OUT L", "OUT R", "EOSG", "CV OUT"]):
    add(text(lab, 112.0 + 18.0 * i, 114.6, 2.9, FONT_BOLD, INK))

svg = (
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
    f'viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n"
)
with open(os.path.join(HERE, "Tapejeans.svg"), "w") as fh:
    fh.write(svg)
print("wrote Tapejeans.svg")
