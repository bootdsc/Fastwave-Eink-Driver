"""Generate the repository hero image: 256x512, four grey levels, no dither.

    python make_hero.py

Writes fastwave-hero.png (the artwork) and fastwave-hero.ppix (a PixelPerfect
project file with the same pixels, one layer, four-entry palette) next to
itself.

Four levels is not a stylistic choice here. It is what an SSD16xx gives you
natively, before any spatial dithering: black, two intermediate states, white.
Every region is a flat fill and nothing is screened, which is also why the
composition leans on silhouette and rim light rather than on shading — with
four tones, edges are all you have. See ../docs/PICTURE.md for what happens
when you try to animate this.
"""

import json
import math
from pathlib import Path

W, H = 256, 512

# 0 black, 1 dark, 2 light, 3 white — the native four.
LEVELS = [0x00, 0x55, 0xAA, 0xFF]
HEX = ["#000000", "#555555", "#AAAAAA", "#FFFFFF"]

fb = [[1 for _ in range(W)] for _ in range(H)]


# ---------------------------------------------------------------- primitives

def px(x, y, t):
    x = int(round(x)); y = int(round(y))
    if 0 <= x < W and 0 <= y < H:
        fb[y][x] = t


def get(x, y):
    x = int(round(x)); y = int(round(y))
    if 0 <= x < W and 0 <= y < H:
        return fb[y][x]
    return -1


def rect(x0, y0, x1, y1, t):
    for y in range(max(0, int(y0)), min(H, int(y1) + 1)):
        row = fb[y]
        for x in range(max(0, int(x0)), min(W, int(x1) + 1)):
            row[x] = t


def ell(cx, cy, rx, ry, t):
    if rx <= 0 or ry <= 0:
        return
    for y in range(int(cy - ry), int(cy + ry) + 1):
        for x in range(int(cx - rx), int(cx + rx) + 1):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            if dx * dx + dy * dy <= 1.0:
                px(x, y, t)


def poly(pts, t):
    """Even-odd scanline fill."""
    if len(pts) < 3:
        return
    ys = [p[1] for p in pts]
    for y in range(int(min(ys)), int(max(ys)) + 1):
        xs = []
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            if y0 == y1:
                continue
            if min(y0, y1) <= y + 0.5 < max(y0, y1):
                xs.append(x0 + (y + 0.5 - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(int(math.floor(xs[i] + 0.5)),
                           int(math.floor(xs[i + 1] + 0.5)) + 1):
                px(x, y, t)


def line(x0, y0, x1, y1, t, w=1):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    r = (w - 1) / 2.0
    for i in range(steps + 1):
        u = i / steps
        cx = x0 + (x1 - x0) * u
        cy = y0 + (y1 - y0) * u
        if w <= 1:
            px(cx, cy, t)
        else:
            for dy in range(int(-r - 1), int(r + 2)):
                for dx in range(int(-r - 1), int(r + 2)):
                    if dx * dx + dy * dy <= r * r + 0.4:
                        px(cx + dx, cy + dy, t)


def taper(pts, t):
    """A limb: list of (x, y, half-width). Filled as stacked discs."""
    for i in range(len(pts) - 1):
        x0, y0, w0 = pts[i]
        x1, y1, w1 = pts[i + 1]
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 2
        for k in range(n + 1):
            u = k / n
            ell(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u,
                w0 + (w1 - w0) * u, w0 + (w1 - w0) * u, t)


def rimlight(x0, y0, x1, y1, t, src, dirx, diry):
    """Put tone t on pixels of tone `src` whose neighbour toward (dirx,diry)
    is not `src`. Cheap directional edge light on a flat silhouette."""
    marks = []
    for y in range(max(1, y0), min(H - 1, y1)):
        for x in range(max(1, x0), min(W - 1, x1)):
            if fb[y][x] != src:
                continue
            if get(x + dirx, y + diry) != src:
                marks.append((x, y))
    for x, y in marks:
        fb[y][x] = t


def rim_from(prev, t, dx, dy, src=0, ylim=H):
    """Rim-light only the pixels a section just drew. Four flat tones give you
    no shading, so the contour is the whole read."""
    mask = [[fb[y][x] != prev[y][x] for x in range(W)] for y in range(H)]
    marks = []
    for y in range(1, min(H - 1, ylim)):
        for x in range(1, W - 1):
            if not mask[y][x] or fb[y][x] != src:
                continue
            nx, ny = x + dx, y + dy
            if not (0 <= nx < W and 0 <= ny < H) or not mask[ny][nx]:
                marks.append((x, y))
    for x, y in marks:
        fb[y][x] = t


def snapshot():
    return [row[:] for row in fb]


def halo(prev, t):
    """Ring anything that changed since `prev` with tone t, on unchanged pixels.

    Cheap way to give a figure a hard edge against whatever it is standing in
    front of, which with four tones is the difference between a silhouette and
    a stain."""
    changed = [[fb[y][x] != prev[y][x] for x in range(W)] for y in range(H)]
    marks = []
    for y in range(1, H - 1):
        for x in range(1, W - 1):
            if changed[y][x]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                           (1, 1), (-1, -1), (1, -1), (-1, 1)):
                if changed[y + dy][x + dx]:
                    marks.append((x, y))
                    break
    for x, y in marks:
        fb[y][x] = t


# ---------------------------------------------------------------- 1. the sky

rect(0, 0, W - 1, H - 1, 1)

# A wall of moonlit haze on the right two-thirds: this is what the creature is
# silhouetted against. Without it, black on dark grey is not a silhouette.
for y in range(0, 400):
    edge = -30 + int(22 * math.sin(y * 0.013 + 0.4))
    for x in range(max(0, edge), W):
        fb[y][x] = 2
# Break the haze up with dark cloud shelves so it is weather, not a rectangle.
for (cy, amp, ph, th) in ((34, 9, 0.0, 14), (78, 12, 1.9, 9),
                          (128, 8, 3.1, 7), (196, 11, 0.8, 8),
                          (258, 9, 2.2, 7), (300, 8, 1.2, 6),
                          (338, 7, 2.7, 6), (372, 6, 0.4, 5)):
    for x in range(W):
        y0 = cy + int(amp * math.sin(x * 0.024 + ph) + 4 * math.sin(x * 0.071 + ph))
        for y in range(y0, y0 + th):
            if get(x, y) in (1, 2):
                px(x, y, 1)
# The very top is airless
for x in range(W):
    h = 16 + int(9 * math.sin(x * 0.05 + 1.0) + 5 * math.sin(x * 0.017))
    for y in range(0, h):
        px(x, y, 0)

# Stars, only where the sky is dark enough to hold them.
STARS = [(9, 24), (23, 33), (37, 20), (51, 44), (63, 28), (19, 58), (44, 70),
         (8, 86), (33, 104), (57, 92), (14, 130), (41, 148), (60, 122),
         (25, 176), (52, 196), (11, 210), (36, 236), (62, 252), (18, 272),
         (46, 292), (72, 62), (86, 30), (112, 22), (140, 34), (168, 26),
         (196, 18), (224, 30), (246, 22), (204, 60), (232, 52)]
for (sx, sy) in STARS:
    if get(sx, sy) in (0, 1):
        px(sx, sy, 3)
    if get(sx, sy) == 2:
        px(sx, sy, 3)
for (sx, sy) in STARS[:8]:
    px(sx + 1, sy, 2)

# A ringed world, low left, cut by the haze so it sits behind the weather.
PX, PY, PR = 52, 96, 33
ell(PX, PY, PR, PR, 3)
for y in range(PY - PR, PY + PR + 1):
    for x in range(PX - PR, PX + PR + 1):
        if get(x, y) == 3:
            d = (x - PX) * 0.66 + (y - PY) * 0.75
            if d > 5:
                px(x, y, 2)
            if d > 21:
                px(x, y, 1)
for k in range(-2, 3):
    yy = PY + k * 12 - 3
    for x in range(PX - PR, PX + PR + 1):
        dy = yy + int(3.0 * math.sin((x - PX) * 0.06))
        if get(x, dy) == 3 and k % 2 == 0:
            px(x, dy, 2)
ell(PX - 11, PY - 15, 6, 4, 2)
ell(PX + 9, PY + 8, 4, 3, 1)
for rx, ry, t in ((52, 12, 3), (45, 10, 2)):
    for a in range(0, 2400):
        th = a * math.pi / 1200.0
        x = PX + rx * math.cos(th) + 4
        y = PY + ry * math.sin(th) + 9
        dd = ((x - PX) / PR) ** 2 + ((y - PY) / PR) ** 2
        if dd < 1.0 and y < PY + 3:
            continue
        px(x, y, t)


# ----------------------------------------------------- 2. ruins and ground

GROUND = 396

# Far colonnade, dark, low contrast, reading as distance.
for (x, top, h, w) in ((196, 250, 146, 13), (218, 276, 120, 10),
                       (238, 262, 134, 12)):
    rect(x, top, x + w, top + h, 1)
    rect(x - 2, top, x + w + 2, top + 5, 1)
    for k in range(4):
        ell(x + (k * 5) % w, top + 10 + (k * 31) % (h - 20), 3, 2, 0)

# A shattered arch across the middle distance, black.
poly([(0, 214), (40, 200), (54, 211), (54, 232), (40, 222), (0, 236)], 0)
for x in range(52, 112):
    u = (x - 52) / 128.0
    y = 211 - int(54 * math.sin(u * math.pi) ** 1.25)
    line(x, y, x, y + 15, 0, 1)
for x in range(146, 198):
    u = (x - 52) / 128.0
    y = 211 - int(54 * math.sin(min(1.0, u) * math.pi) ** 1.25)
    line(x, y, x, y + 14, 0, 1)
for (rx, ry, rr) in ((122, 176, 4), (134, 190, 3), (117, 194, 3),
                     (140, 170, 5), (128, 206, 2)):
    ell(rx, ry, rr, rr - 1, 0)

# Two near columns, black, framing the plate. Broken, fluted, huge.
for (x, top, h, w) in ((-16, 150, 250, 20), (232, 128, 272, 30)):
    rect(x, top, x + w, top + h, 0)
    rect(x - 5, top, x + w + 5, top + 11, 0)
    for fx in range(x + 5, x + w - 2, 7):
        rect(fx, top + 15, fx + 1, top + h - 6, 1)
    for k in range(12):
        ell(x - 5 + (k * 3) % (w + 10), top - 2 + (k % 4), 3, 2,
            2 if k % 3 == 0 else 0)

# Ground: black, with a band of lit dust at the horizon.
rect(0, GROUND, W - 1, H - 1, 1)
for x in range(W):
    y = GROUND + int(4 * math.sin(x * 0.055)) - 3
    line(x, y, x, y + 4, 2, 1)
    line(x, y + 4, x, y + 8, 0, 1)
# Flagstone joints converging on the vanishing point
for k in range(11):
    x0 = -90 + k * 44
    line(x0, H + 8, 118 + (x0 - 118) // 8, GROUND + 6, 0, 1)
for k in range(6):
    yy = GROUND + 9 + int(k * k * 3.4)
    for x in range(0, W, 2):
        if (x + k * 3) % 13 > 2:
            px(x, yy, 0)
# Broken slabs, lit on top, black underneath
for (rx, ry, a, b) in ((26, 456, 20, 7), (196, 466, 24, 9), (236, 496, 22, 8),
                       (150, 502, 30, 10), (118, 446, 13, 5)):
    ell(rx, ry, a, b, 0)
    poly([(rx - a, ry), (rx - a // 2, ry - b), (rx + a // 2, ry - b + 1),
          (rx + a, ry + 1)], 2)


# ------------------------------------------ 3. the thing, upper right
#
# Black against the haze. The only bright things on it are its eyes and its
# teeth, which are also the two things you would rather not have found.

snap = snapshot()

# --- wings, spread behind everything else it owns
for sgn in (-1, 1):
    ax, ay = 180, 274                                    # shoulder joint
    knux, knuy = ax + sgn * 62, ay - 116                 # wrist
    tips = [(ax + sgn * 128, knuy + 6), (ax + sgn * 116, knuy + 74),
            (ax + sgn * 86, knuy + 128), (ax + sgn * 44, knuy + 150)]
    pts = [(ax, ay), (ax + sgn * 26, ay - 66), (knux, knuy)]
    for i, (tx, ty) in enumerate(tips):
        pts.append((tx, ty))
        if i < len(tips) - 1:
            nx, ny = tips[i + 1]
            pts.append(((tx + nx) / 2 - sgn * 22, (ty + ny) / 2 - 16))
    pts.append((ax + sgn * 18, ay - 16))
    poly(pts, 0)
    for (tx, ty) in tips:                                # finger bones
        line(knux, knuy + 2, tx, ty, 1, 2)
    line(ax, ay, knux, knuy, 1, 3)
    line(ax + sgn * 22, ay - 56, knux - sgn * 4, knuy + 8, 1, 1)

# --- body: hunched, sunk into the ruins
poly([(132, 400), (140, 322), (152, 272), (186, 254), (222, 268),
      (238, 320), (248, 400)], 0)
for k in range(6):                                       # carapace ridges
    y = 288 + k * 18
    line(146 - k, y, 232 + k * 2, y + 7, 1, 2)
poly([(150, 268), (190, 252), (224, 266), (208, 282), (166, 280)], 0)

# --- neck, leaning the head out over the ground to the left
taper([(178, 272, 20), (162, 246, 17), (146, 224, 15)], 0)
line(166, 250, 150, 228, 1, 2)

# --- skull: long, snout down and to the left, jaw open
poly([(150, 206), (176, 200), (188, 216), (176, 246), (146, 252),
      (124, 240), (108, 228), (120, 214)], 1)
poly([(152, 212), (172, 207), (182, 220), (170, 240), (146, 245),
      (126, 235), (116, 227), (126, 218)], 0)
line(126, 224, 176, 214, 1, 1)                           # brow ridge

# eyes: three, white, and none of them is looking anywhere pleasant
for (ex, ey, er) in ((136, 224, 4), (156, 219, 4), (146, 232, 3)):
    ell(ex, ey, er, er, 3)
    px(ex - 1, ey, 0)
    px(ex, ey, 0)

# jaw, dropped open under the snout
poly([(112, 236), (150, 250), (152, 264), (116, 252)], 1)
poly([(117, 240), (146, 251), (147, 259), (120, 248)], 0)
for k in range(8):                                       # upper teeth
    tx, ty = 114 + k * 8, 232 + k * 2
    poly([(tx, ty), (tx + 5, ty + 1), (tx + 2, ty + 12)], 3)
for k in range(7):                                       # lower teeth
    tx, ty = 118 + k * 8, 252 + k * 2
    poly([(tx, ty), (tx + 5, ty + 1), (tx + 3, ty - 11)], 2)

# --- horns: two big ones sweeping back over the wings, two small
poly([(168, 202), (196, 150), (222, 104), (206, 152), (180, 200)], 0)
poly([(154, 200), (172, 152), (192, 118), (178, 156), (162, 200)], 0)
poly([(178, 212), (204, 190), (222, 178), (200, 200), (182, 218)], 0)
poly([(146, 204), (140, 172), (150, 188), (154, 204)], 0)
line(172, 198, 216, 112, 1, 1)
line(158, 198, 186, 126, 1, 1)
line(182, 210, 218, 182, 1, 1)

# --- near arm, down the front of the body, claws in the near rubble
taper([(168, 292, 15), (170, 330, 13), (178, 362, 11), (184, 386, 9)], 0)
for k in range(5):
    a = -1.5 + k * 0.44
    line(184, 386, 184 + math.cos(a) * 26, 386 + math.sin(a) * 26 + 14, 0, 5)
    line(184, 386, 184 + math.cos(a) * 26, 386 + math.sin(a) * 26 + 14, 2, 2)
    line(184 + math.cos(a) * 21, 386 + math.sin(a) * 21 + 12,
         184 + math.cos(a) * 28, 386 + math.sin(a) * 28 + 15, 3, 1)
line(166, 298, 168, 332, 1, 2)
line(170, 336, 178, 364, 1, 2)

# --- far arm, planted in the rubble
taper([(228, 288, 13), (248, 334, 11), (252, 380, 10)], 0)
line(226, 294, 246, 338, 1, 2)

halo(snap, 0)                                            # separate it cleanly


# --------------------------------------- 4. the one doing something about it
#
# Bottom left, mid-throw, weight over the metal leg. Smaller than the thing he
# is throwing at, which is the whole point of the picture.

snap = snapshot()

GX, GY = 54, 394          # pelvis centre

poly([(0, 404), (124, 400), (150, 512), (0, 512)], 2)
for k in range(9):
    line(-40 + k * 26, 512, 30 + k * 11, 402, 0, 1)
for k in range(4):
    yy = 416 + k * 26 + k * k * 3
    for x in range(0, 152, 2):
        if (x + k * 4) % 14 > 3:
            px(x, yy + int(3 * math.sin(x * 0.04)), 0)
ell(GX - 4, 492, 58, 12, 0)
poly([(GX - 62, 492), (GX - 26, 480), (GX + 34, 482), (GX + 52, 494)], 1)

# ---------- everything from here is one black mass, rim-lit at the end -------
#
# Silhouette first, then negative space punched back out of it. With four flat
# tones a figure is only ever as readable as the gaps between its limbs.

sil = snapshot()

# back leg, braced
taper([(GX - 10, GY + 4, 11), (GX - 30, GY + 46, 9), (GX - 50, GY + 82, 7)], 0)
poly([(GX - 66, GY + 82), (GX - 40, GY + 82), (GX - 36, GY + 94),
      (GX - 70, GY + 94)], 0)

# torso: hips, waist, chest, shoulders
poly([(GX - 18, GY + 4), (GX + 20, GY + 2), (GX + 18, GY - 20),
      (GX + 24, GY - 50), (GX + 28, GY - 58), (GX - 24, GY - 58),
      (GX - 20, GY - 50), (GX - 16, GY - 20)], 0)
poly([(GX + 6, GY - 56), (GX + 18, GY - 56), (GX + 18, GY - 66),
      (GX + 6, GY - 66)], 0)                              # neck

# head and crest
ell(GX + 15, GY - 78, 13, 14, 0)
poly([(GX + 4, GY - 90), (GX + 20, GY - 94), (GX + 46, GY - 124),
      (GX + 30, GY - 98), (GX + 12, GY - 88)], 0)

# arms
taper([(GX + 22, GY - 54, 10), (GX + 44, GY - 78, 8), (GX + 60, GY - 104, 6)], 0)
ell(GX + 62, GY - 107, 7, 7, 0)
taper([(GX - 18, GY - 52, 10), (GX - 38, GY - 36, 8), (GX - 50, GY - 26, 6)], 0)
ell(GX - 52, GY - 25, 6, 5, 0)

halo(sil, 0)

# ---- negative space, cut straight back to the sky behind him
poly([(GX - 2, GY + 18), (GX + 8, GY + 20), (GX + 12, GY + 64),
      (GX - 16, GY + 68)], 2)                             # between the legs
poly([(GX + 1, GY - 60), (GX + 5, GY - 60), (GX + 5, GY - 68),
      (GX + 1, GY - 68)], 2)                              # neck, near side
poly([(GX + 19, GY - 60), (GX + 24, GY - 60), (GX + 24, GY - 70),
      (GX + 19, GY - 70)], 2)                             # neck, far side
poly([(GX + 22, GY - 52), (GX + 34, GY - 62), (GX + 40, GY - 54),
      (GX + 26, GY - 44)], 2)                             # under the raised arm
poly([(GX - 20, GY - 48), (GX - 30, GY - 41), (GX - 24, GY - 31),
      (GX - 16, GY - 40)], 2)                             # under the back arm

rim_from(sil, 2, 1, -1, ylim=GY - 4)                      # sky edge only
rim_from(sil, 3, 2, -2, ylim=GY - 40)

# ---- the leg that is not his: plate, actuator, piston, claw. The only part of
# him drawn in anything but black, because it is the reason he is upright.
sil2 = snapshot()
poly([(GX + 4, GY - 6), (GX + 30, GY + 2), (GX + 34, GY + 46),
      (GX + 14, GY + 50)], 2)
poly([(GX + 11, GY + 2), (GX + 25, GY + 9), (GX + 28, GY + 42),
      (GX + 17, GY + 44)], 1)
line(GX + 8, GY + 8, GX + 31, GY + 15, 3, 1)
line(GX + 4, GY - 6, GX + 14, GY + 50, 0, 1)
ell(GX + 24, GY + 52, 10, 9, 2)
ell(GX + 24, GY + 52, 6, 5, 0)
ell(GX + 24, GY + 52, 2, 2, 3)
taper([(GX + 25, GY + 60, 7), (GX + 32, GY + 94, 6)], 2)
line(GX + 25, GY + 60, GX + 32, GY + 94, 0, 1)
for k in range(6):
    yy = GY + 64 + k * 6
    line(GX + 18, yy, GX + 39, yy + 1, 3, 1)
poly([(GX + 20, GY + 96), (GX + 44, GY + 96), (GX + 48, GY + 106),
      (GX + 16, GY + 106)], 2)
for k in range(3):
    poly([(GX + 19 + k * 9, GY + 105), (GX + 27 + k * 9, GY + 105),
          (GX + 23 + k * 9, GY + 115)], 3)
line(GX + 16, GY + 106, GX + 48, GY + 106, 0, 1)
halo(sil2, 0)

# ---- and the two marks that make the head a head
poly([(GX + 14, GY - 84), (GX + 27, GY - 86), (GX + 28, GY - 79),
      (GX + 15, GY - 77)], 3)                             # visor slot
poly([(GX + 17, GY - 83), (GX + 22, GY - 84), (GX + 22, GY - 80),
      (GX + 17, GY - 79)], 0)                             # its one dark bar
line(GX + 4, GY - 88, GX + 12, GY - 90, 2, 1)             # brow
line(GX + 6, GY - 68, GX + 20, GY - 66, 2, 1)             # chin guard
line(GX + 22, GY - 94, GX + 46, GY - 123, 3, 2)           # crest edge
line(GX + 16, GY - 92, GX + 34, GY - 108, 2, 1)


# ------------------------------------------------ 5. the lance, in flight
#
# Released, and most of the way there. The brightest thing on the plate.

LX0, LY0 = GX + 70, GY - 116        # just past the open hand
LX1, LY1 = 134, 226                 # the near eye socket

line(LX0, LY0, LX1, LY1, 0, 9)      # its own shadow, for separation
line(LX0, LY0, LX1, LY1, 3, 3)
line(LX0 + 1, LY0 - 2, LX1 + 1, LY1 - 2, 2, 1)

ang = math.atan2(LY1 - LY0, LX1 - LX0)
ca, sa = math.cos(ang), math.sin(ang)
hx, hy = LX1, LY1
poly([(hx, hy),
      (hx - 20 * ca - 8 * sa, hy - 20 * sa + 8 * ca),
      (hx - 27 * ca, hy - 27 * sa),
      (hx - 20 * ca + 8 * sa, hy - 20 * sa - 8 * ca)], 3)
poly([(hx - 24 * ca, hy - 24 * sa),
      (hx - 40 * ca - 12 * sa, hy - 40 * sa + 12 * ca),
      (hx - 34 * ca, hy - 34 * sa)], 2)
poly([(hx - 24 * ca, hy - 24 * sa),
      (hx - 40 * ca + 12 * sa, hy - 40 * sa - 12 * ca),
      (hx - 34 * ca, hy - 34 * sa)], 2)

for k in range(5):                  # coil pack along the shaft
    u = 0.34 + k * 0.10
    mx = LX0 + (LX1 - LX0) * u
    my = LY0 + (LY1 - LY0) * u
    line(mx - 5 * sa, my + 5 * ca, mx + 5 * sa, my - 5 * ca, 3, 2)
    line(mx - 6 * sa + 3 * ca, my + 6 * ca + 3 * sa,
         mx + 6 * sa + 3 * ca, my - 6 * ca + 3 * sa, 0, 1)

for k in range(20):                 # ionisation trail, back past the hand
    u = -0.03 - k * 0.048
    mx = LX0 + (LX1 - LX0) * u
    my = LY0 + (LY1 - LY0) * u
    t = 3 if k < 4 else (2 if k < 12 else 1)
    ln = max(2, 9 - k // 3)
    line(mx, my, mx - ln * ca, my - ln * sa, t, 2 if k < 8 else 1)

# impact: a flare in the socket and cracks running out through bone
ell(LX1 + 1, LY1, 9, 9, 3)
for k in range(12):
    a = k * math.pi / 6 + 0.26
    r = 14 + (k % 3) * 8
    line(LX1 + 1, LY1, LX1 + 1 + math.cos(a) * r, LY1 + math.sin(a) * r, 3, 1)
for k in range(8):
    a = k * math.pi / 4 + 0.8
    line(LX1 + 1 + math.cos(a) * 12, LY1 + math.sin(a) * 12,
         LX1 + 1 + math.cos(a) * 28, LY1 + math.sin(a) * 28, 2, 1)
ell(LX1 + 1, LY1, 4, 4, 3)


# -------------------------------------------- 6. edge light, then the border
#
# One directional pass picks out the top-left edge of every black mass, which
# is the only thing keeping the ruins, the beast and the ground apart.

rimlight(0, 130, W, 396, 1, 0, -1, -1)
rect(0, 0, W - 1, 0, 0)
rect(0, H - 1, W - 1, H - 1, 0)
rect(0, 0, 0, H - 1, 0)
rect(W - 1, 0, W - 1, H - 1, 0)


# ------------------------------------------------------------------- output

here = Path(__file__).resolve().parent

try:
    from PIL import Image
except ImportError:
    raise SystemExit("Pillow required: pip install pillow")

img = Image.new("L", (W, H))
img.putdata([LEVELS[fb[y][x]] for y in range(H) for x in range(W)])
img.save(here / "fastwave-hero.png")

# PixelPerfect project. Index 0 is that editor's transparent slot, so the four
# tones live at 1..4 and nothing uses 0.
doc = {
    "format": "ppix",
    "version": 2,
    "width": W,
    "height": H,
    "palette": ["#000000"] + HEX,
    "active": 0,
    "layers": [{
        "name": "hero",
        "visible": True,
        "pixels": [[fb[y][x] + 1 for x in range(W)] for y in range(H)],
    }],
}
(here / "fastwave-hero.ppix").write_text(json.dumps(doc), encoding="utf-8")

hist = [0, 0, 0, 0]
for row in fb:
    for v in row:
        hist[v] += 1
print("wrote fastwave-hero.png and fastwave-hero.ppix")
print("tone histogram (black/dark/light/white):", hist)
