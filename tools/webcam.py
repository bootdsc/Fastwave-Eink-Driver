"""fastwave / Ink Flow host — live webcam / clip to e-ink.

  python ink_flow.py
  python webcam.py COM19 --camera 1
  python webcam.py COM19 --source clip.mp4
  python webcam.py COM17 --panel crow-579 --source clip.mp4

Panels:
  xiao-426   4.26" SSD1677    (ports/ssd1677-seeed-426)   default
  crow-579   5.79" dual SSD1683 (ports/ssd1683-crowpanel-579)
Rules: ../docs/PITFALLS.md and ../docs/STREAMING.md

Keys in this window:
  [ ]  1-9 0   drive (rearm)
  , .          volts 15/16/17 (rearm)
  t T          trail 0..12 (rearm)
  e E          chase every 1..8 (rearm)
  d D          gray levels 2 / 4 / 16 / 64 (live)
  b B          dither bias more black / more white (live)
  g G c C      gamma / contrast (live)
  h H k K      highlights / knee (live)
  i m p        invert / mirror / pause
  w            white + rejoin
  q ESC        quit
"""
import argparse
import collections
import ctypes
import os
import queue
import re
import sys
import threading
import time

import numpy as np
import serial
from PIL import Image, ImageDraw, ImageFont, ImageGrab

try:
    import cv2
except ImportError:
    sys.exit("pip install opencv-python")
try:
    import msvcrt
except ImportError:
    msvcrt = None
try:
    import tkinter as tk
except ImportError:
    tk = None
try:
    import serial.tools.list_ports
except ImportError:
    serial.tools = None  # type: ignore

CROW_TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "ports", "ssd1683-crowpanel-579", "tools")
if CROW_TOOLS not in sys.path:
    sys.path.insert(0, CROW_TOOLS)
def fail(msg):
    # Console only. This used to also write tools/last_stream.log; a log file
    # outlives the run and can carry source paths, so it is gone.
    print(msg)
    sys.exit(1)

# Native controller RAM: 800×480 landscape. StarPilot stands the panel
# portrait; logical canvas is 480×800, mapped (nx, ny) = (799-y, x).
# 1.54" is square 200×200, no portrait map.
NW, NH = 800, 480
LW, LH = 480, 800
STRIDE = NW // 8
PLANE = STRIDE * NH
CHUNK = 48000
TOKEN = bytes([0x06])
CAN = bytes([0x18])
IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".gif"}


class Geo:
    __slots__ = ("name", "nw", "nh", "lw", "lh", "stride", "plane", "portrait")

    def __init__(self, name, nw, nh, lw, lh, portrait):
        self.name = name
        self.nw, self.nh = nw, nh
        self.lw, self.lh = lw, lh
        self.stride = nw // 8
        self.plane = self.stride * nh
        self.portrait = portrait


GEO = {
    "xiao-426": Geo("xiao-426", 800, 480, 480, 800, True),
    "xiao-154": Geo("xiao-154", 200, 200, 200, 200, False),
}


def geo_for(panel):
    return GEO.get(panel, GEO["xiao-426"])

# Host tone. Do not retune unless the user sets a 0-255 knob.
# 128 = linear / unity. See print_knobs().
GAMMA = 1.30
CONTRAST = 1.40
WHITE_PT = 225
# Below this: solid black, no dither. 0 left every dark area speckled.
BLACK_PT = 40
DITHER = 4
# 128 = 50/50 at mid-grey. Lower = more black cells (the 4x4 was too white).
BIAS = 64
DRIVE = 3
# Reuse last bit if gray moved less than this — stops cam noise flipping
# every pixel every frame.
HOLD = 12
INVERT = False
# TinyUSB CDC ignores UART baud for throughput (USB FS bulk). 115200 is the
# monitor rate. Host open uses the same number so a CH340 path still talks.
BAUD = 115200
# 4x4 clustered (16 steps). User set dither=16; 8x8/64 is CLUSTER8 in comments.
# Values 0..15, same-byte neighbours stay together so static areas delta-drop.
DITHER_STEPS = (2, 4, 16, 64)
CLUSTER2 = np.array([
    [0, 1],
    [3, 2],
], dtype=np.float32)
CLUSTER4 = np.array([
    [ 0,  1,  4,  5],
    [ 3,  2,  7,  6],
    [12, 13,  8,  9],
    [15, 14, 11, 10],
], dtype=np.float32)
CLUSTER8 = np.array([
    [24, 10, 12, 26, 35, 47, 49, 37],
    [ 8,  0,  2, 14, 45, 59, 61, 51],
    [22,  6,  4, 16, 43, 57, 63, 53],
    [30, 20, 18, 28, 33, 41, 55, 39],
    [34, 46, 48, 36, 25, 11, 13, 27],
    [44, 58, 60, 50,  9,  1,  3, 15],
    [42, 56, 62, 52, 23,  7,  5, 17],
    [32, 40, 54, 38, 31, 21, 19, 29],
], dtype=np.float32)
VOLTS = ("41 32", "46 36", "4B 3A")
VOLT_N = ("+/-15V", "+/-16V", "+/-17V")

_FPS_FONT = None


def fps_font(size=72):
    global _FPS_FONT
    if _FPS_FONT is not None and _FPS_FONT.size == size:
        return _FPS_FONT
    for path in (
        r"C:\Windows\Fonts\arialbd.ttf",
        r"C:\Windows\Fonts\arial.ttf",
        r"C:\Windows\Fonts\consolab.ttf",
        r"C:\Windows\Fonts\consola.ttf",
    ):
        try:
            _FPS_FONT = ImageFont.truetype(path, size)
            return _FPS_FONT
        except OSError:
            continue
    _FPS_FONT = ImageFont.load_default()
    return _FPS_FONT


def make_fps_plate(fps):
    """Rasterize the badge once. Call only when the number changes."""
    font = fps_font(72)
    label = f"{fps:4.1f}"
    scratch = Image.new("L", (8, 8), 255)
    sdr = ImageDraw.Draw(scratch)
    try:
        l, t, r, b = sdr.textbbox((0, 0), label, font=font)
        tw, th = r - l, b - t
        ox, oy = -l, -t
    except AttributeError:
        tw, th = sdr.textsize(label, font=font)
        ox, oy = 0, 0
    pad, border = 10, 6
    box_w = tw + pad * 2
    box_h = th + pad * 2
    plate = Image.new("L", (box_w, box_h), 255)
    pdr = ImageDraw.Draw(plate)
    for i in range(border):
        pdr.rectangle((i, i, box_w - 1 - i, box_h - 1 - i), outline=0)
    pdr.text((pad + ox, pad + oy), label, fill=0, font=font)
    # ROTATE_90 was upside-down on the standing panel. 270 = 90 CW.
    plate = plate.transpose(Image.ROTATE_270)
    return np.asarray(plate, dtype=np.uint8)


def blit_fps_plate(gray, plate, portrait):
    """Cheap slice write. No PIL. Sits in the video pane, not the blank band."""
    ph, pw = plate.shape
    h = gray.shape[0]
    if portrait:
        y0, x0 = 6, 6
    else:
        y0, x0 = max(0, h - ph - 6), 6
    if y0 + ph > h or x0 + pw > gray.shape[1]:
        return gray
    gray[y0:y0 + ph, x0:x0 + pw] = plate
    return gray


# Frame period in ms per FR code (SSD1683/SSD1681 Table 6-7). FR is a frame-rate
# TARGET, not a consequence of gate count -- 0x3A/0x3B were swept across their
# whole range on the 1.54" and moved the period by less than the probe's own
# +/-1 us. So the code alone fixes the period, and this table is the whole law.
#
# 0x04 is measured (10.076 ms), not nominal; the rest are 1000/nominal Hz and
# carry the same ~0.8% overhead in practice.
FR_PERIOD_MS = {
    0x01: 40.00, 0x02: 20.00, 0x03: 13.33, 0x04: 10.076, 0x05: 8.063,
    0x09: 26.67, 0x0A: 16.00, 0x0B: 11.43, 0x0C: 8.889,
}
FR_DEFAULT = 0x04
SPI_HZ_DEFAULT = 40_000_000

# Everything that is neither waveform nor wire: the host's per-frame work and
# the USB round trip. The old hardcoded 4.0 ms "SPI" term quietly carried this,
# which is why splitting the wire out on its own made the estimate optimistic
# rather than accurate.
#
# FITTED, then checked out of sample. Drive 1 measures 45.9-46.1 fps = 21.74 ms
# against 2 x 10.076 ms of waveform and 1.0 ms of wire, leaving 0.6 ms.
#
# Fitting one constant to one point proves nothing on its own -- the match is
# what fitting means. The check is a drive count that was NOT fitted: drive 3
# predicts 23.9 fps and measures 23.9. That doubles the waveform term while
# wire and overhead stay fixed, so a 0.6 that was really absorbing per-frame
# cost would have drifted there. It did not, which is what makes the split
# between per-frame and fixed cost trustworthy rather than convenient.
#
# Still only two points on one panel. It is a good constant, not a law.
HOST_OVERHEAD_MS = 0.6


def panel_fps(drive, geo=None, fr=None, spi_hz=None):
    """Predicted panel fps for a custom-waveform drive.

    Both the frame period and the wire time used to be hardcoded here -- 8.063
    ms (FR 0x05) and 4 ms (one plane at 20 MHz). The port runs FR 0x04 at 40
    MHz, so the readout sat ~4 fps optimistic against the panel beside it. Both
    are derived now, so this stays honest when either is changed.

    Only valid with `fast on`. The OTP partial is a flat 363 ms regardless.
    """
    if geo is not None and geo.name == "xiao-154":
        period = FR_PERIOD_MS.get(fr if fr else FR_DEFAULT,
                                  FR_PERIOD_MS[FR_DEFAULT])
        # One 5000-byte plane. 5000 x 8 bits / Hz, in milliseconds.
        hz = spi_hz if spi_hz else SPI_HZ_DEFAULT
        wire = 40_000_000.0 / float(hz)
        return 1000.0 / ((drive + 1) * period + wire + HOST_OVERHEAD_MS)
    return 1000.0 / ((drive + 1) * 9.92 + 20.0)


def _extend_edges(arr, ox, oy, nw, nh):
    """Letterbox bars filled with the picture's edge pixels, not a solid field.

    A solid white (or black) bar is idle ink. Every DISPLAY still scans it,
    skipped LUT phases dump DC into it, and an hour of that is burn-in.
    Repeating the edge row/column keeps those pixels in the same motion as
    the frame, so they get a real drive when the picture moves.
    """
    out = np.array(arr, copy=True)
    h, w = out.shape
    ix0, iy0 = ox, oy
    ix1, iy1 = ox + nw, oy + nh
    if iy0 > 0:
        out[:iy0, ix0:ix1] = out[iy0, ix0:ix1]
    if iy1 < h:
        out[iy1:, ix0:ix1] = out[iy1 - 1, ix0:ix1]
    if ix0 > 0:
        out[iy0:iy1, :ix0] = out[iy0:iy1, ix0:ix0 + 1]
    if ix1 < w:
        out[iy0:iy1, ix1:] = out[iy0:iy1, ix1 - 1:ix1]
    if iy0 > 0 and ix0 > 0:
        out[:iy0, :ix0] = out[iy0, ix0]
    if iy0 > 0 and ix1 < w:
        out[:iy0, ix1:] = out[iy0, ix1 - 1]
    if iy1 < h and ix0 > 0:
        out[iy1:, :ix0] = out[iy1 - 1, ix0]
    if iy1 < h and ix1 < w:
        out[iy1:, ix1:] = out[iy1 - 1, ix1 - 1]
    return out


def fit_wh(im, tw, th, letterbox=False, pad=0):
    if im.mode != "L":
        im = im.convert("L")
    if im.width == tw and im.height == th:
        return np.asarray(im)
    if letterbox:
        scale = min(tw / im.width, th / im.height)
        nw = max(1, int(im.width * scale + 0.5))
        nh = max(1, int(im.height * scale + 0.5))
        ox, oy = (tw - nw) // 2, (th - nh) // 2
        im = im.resize((nw, nh), Image.BILINEAR)
        if int(pad) < 0:
            canvas = Image.new("L", (tw, th), 0)
            canvas.paste(im, (ox, oy))
            return _extend_edges(np.asarray(canvas), ox, oy, nw, nh)
        canvas = Image.new("L", (tw, th), int(pad) & 255)
        canvas.paste(im, (ox, oy))
        return np.asarray(canvas)
    scale = max(tw / im.width, th / im.height)
    im = im.resize((max(tw, int(im.width * scale + 0.5)),
                    max(th, int(im.height * scale + 0.5))), Image.BILINEAR)
    left = (im.width - tw) // 2
    top = (im.height - th) // 2
    return np.asarray(im.crop((left, top, left + tw, top + th)))


def to_native(logical):
    """480×800 logical (StarPilot) → 480×800 numpy is (800,480); native (480,800)."""
    # N[x, 799-y] = L[y, x]  →  N = L.T[:, ::-1]
    return np.ascontiguousarray(logical.T[:, ::-1])


def compose_portrait(im, letterbox=False):
    """Full 480×800 logical, centre-crop to fill (StarPilot map)."""
    return to_native(fit_wh(im, LW, LH, letterbox=letterbox, pad=0))


def compose_geo(im, knobs, geo):
    """Source frame -> panel-native plane, with framing applied.

    fit   True  = letterbox, whole frame visible, bars in `pad`
          False = centre-crop to fill, edges lost
    zoom  percent, 100 = none. Crops a centred window first, so it composes
          with either fit mode: zoomed letterbox still shows bars.
    pad   0 black / 255 white / -1 extend (repeat edge pixels).
          Solid bars are idle ink and DC-creep; extend is the long-run
          option. Solid black/white still pulse to the opposite polarity
          for ~0.25 s every minute so they do not sit skipped for an hour.

    All three are read per frame by the encoder thread, so they are live.
    """
    if knobs.mirror:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    z = int(getattr(knobs, "zoom", 100) or 100)
    if z > 100:
        w, h = im.size
        cw, ch = max(1, w * 100 // z), max(1, h * 100 // z)
        left, top = (w - cw) // 2, (h - ch) // 2
        im = im.crop((left, top, left + cw, top + ch))
    pad = int(getattr(knobs, "pad", 0) or 0)
    if knobs.fit and pad in (0, 255):
        # Invert solid bars briefly so those pixels take a real B↔W round
        # trip instead of a skipped LUT phase every frame.
        t = time.time()
        period = float(getattr(knobs, "bar_pulse_s", 60.0) or 60.0)
        hold = float(getattr(knobs, "bar_pulse_hold", 0.25) or 0.25)
        if period > 0 and (t % period) < hold:
            pad = 255 - pad
    if geo.portrait and not knobs.landscape:
        return to_native(fit_wh(im, geo.lw, geo.lh, knobs.fit, pad=pad))
    return fit_wh(im, geo.nw, geo.nh, knobs.fit, pad=pad)


def test_card(geo=None):
    geo = geo or GEO["xiao-426"]
    if geo.portrait:
        lh, lw = geo.lh, geo.lw
        g = np.full((lh, lw), 40, dtype=np.uint8)
        g[20:60, :] = 255
        g[lh - 60:lh - 20, :] = 255
        g[:, 20:50] = 255
        cy, cx, r = lh // 2, lw // 2, max(12, min(lh, lw) // 8)
        yy, xx = np.ogrid[:lh, :lw]
        g[(yy - cy) ** 2 + (xx - cx) ** 2 < r * r] = 255
        g[cy - 8:cy + 8, cx - 70:cx + 70] = 0
        return to_native(g)
    h, w = geo.nh, geo.nw
    g = np.full((h, w), 40, dtype=np.uint8)
    m = max(4, h // 20)
    g[m:m * 3, :] = 255
    g[h - m * 3:h - m, :] = 255
    g[:, m:m * 2] = 255
    cy, cx, r = h // 2, w // 2, max(8, min(h, w) // 5)
    yy, xx = np.ogrid[:h, :w]
    g[(yy - cy) ** 2 + (xx - cx) ** 2 < r * r] = 255
    return g


def apply_tone(g, gamma, contrast, highlights, knee):
    g = np.asarray(g, dtype=np.float32).copy()
    if gamma != 1.0:
        g = 255.0 * np.power(g / 255.0, gamma)
    if contrast != 1.0:
        g = (g - 128.0) * contrast + 128.0
    g = np.clip(g, 0.0, 255.0)
    if highlights < 255:
        m = g > knee
        g[m] = knee + (g[m] - knee) * (highlights - knee) / (255.0 - knee)
    return g


def dither_step(levels, delta):
    steps = DITHER_STEPS
    try:
        i = steps.index(int(levels))
    except ValueError:
        i = 2 if int(levels) >= 16 else 0
    return steps[max(0, min(len(steps) - 1, i + delta))]


def build_threshold(levels=DITHER, bias=BIAS, nh=NH, nw=NW):
    lv = int(levels)
    if lv <= 2:
        cell, n = np.array([[0]], dtype=np.float32), 1.0
    elif lv <= 4:
        cell, n = CLUSTER2, 4.0
    elif lv <= 16:
        cell, n = CLUSTER4, 16.0
    else:
        cell, n = CLUSTER8, 64.0
    # Shift every threshold up when bias < 128 so fewer cells go white.
    m = (cell + 0.5) / n * 255.0 + (128.0 - float(bias))
    m = np.clip(m, 1.0, 254.0)
    th, tw = m.shape
    tiled = np.tile(m, (nh // th + 1, nw // tw + 1))[:nh, :nw]
    return np.clip(tiled, 1, 254).astype(np.uint8)


def build_lut(gamma, contrast, white=WHITE_PT, black=BLACK_PT):
    i = np.arange(256, dtype=np.float32) / 255.0
    v = np.power(i, gamma)
    v = (v - 0.5) * contrast + 0.5
    out = np.clip(v * 255.0, 0, 255).astype(np.uint8)
    if black > 0:
        out[out <= black] = 0
    if white < 255:
        out[out >= white] = 255
    return out


def encode_delta(cur, prev):
    d = cur != prev
    return np.packbits(d).tobytes(), cur[d].tobytes()


def rle_occupancy(bmp):
    """0x00, count(1..255) zero runs; other bytes literal. Matches firmware."""
    out = bytearray()
    n = len(bmp)
    i = 0
    while i < n:
        if bmp[i] == 0:
            z = 1
            i += 1
            while i < n and bmp[i] == 0 and z < 255:
                z += 1
                i += 1
            out.append(0)
            out.append(z)
        else:
            out.append(bmp[i])
            i += 1
    return bytes(out)


def pack_wire2(bmp, lit):
    rle = rle_occupancy(bmp)
    if len(rle) < len(bmp):
        flags, occ = 1, rle
    else:
        flags, occ = 0, bmp
    n = len(lit)
    return bytes((flags, n & 255, (n >> 8) & 255)) + occ + lit


def pack_wire_banner(raw):
    """Read the wire version off a banner, defaulting to 2.

    Every firmware in this tree is STREAM_WIRE 2. The banner can arrive
    truncated (see stream_handshake), and defaulting a truncated read to 1 used
    to speak legacy framing at a wire=2 chip: one static frame, then a hang.
    Downgrade only on an explicit wire=1.
    """
    if isinstance(raw, str):
        raw = raw.encode("utf-8", "replace")
    if b"wire=1" in raw:
        return 1
    return 2


PANEL_RE = re.compile(rb"panel=([A-Za-z0-9_\-]+)")
PANEL_KEYS = ("xiao-426", "xiao-154", "crow-579")
FR_RE = re.compile(rb"\bfr=0x([0-9A-Fa-f]{2})")
SPIHZ_RE = re.compile(rb"\bspi=\S+\s+\S+\s+(\d+)Hz")


def timing_from_text(raw):
    """Pull the FR code and SPI clock out of a STATUS blob.

    Returns (fr, spi_hz), either of which may be None. The device is the only
    authority on both: the host's defaults are a guess that goes stale the
    moment a boot constant or a live `fr` changes, and a wrong one here shows
    up as an fps readout that disagrees with the panel next to it.
    """
    if isinstance(raw, str):
        raw = raw.encode("utf-8", "replace")
    raw = raw or b""
    m = FR_RE.search(raw)
    fr = int(m.group(1), 16) if m else None
    m = SPIHZ_RE.search(raw)
    hz = int(m.group(1)) if m else None
    return fr, hz


def panel_from_text(raw):
    """Pull `panel=<env>` out of a STATUS line or a stream banner.

    The firmware knows which panel it was built for; the USB VID does not.
    Guessing from the VID is what pushed a 48000-byte plane into a 5000-byte
    controller for an entire session.
    """
    if isinstance(raw, str):
        raw = raw.encode("utf-8", "replace")
    m = PANEL_RE.search(raw or b"")
    if not m:
        return None
    key = m.group(1).decode("ascii", "replace")
    return key if key in PANEL_KEYS else None


def confirm_panel(s, knobs, forced):
    """Reconcile the host's packing geometry with what the device reports.

    Returns True to proceed. A forced panel that contradicts the device is a
    hard stop: the plane sizes differ by 10x and the result is pure noise.
    """
    knobs.fr = getattr(s, "ink_fr", None)
    knobs.spi_hz = getattr(s, "ink_spi_hz", None)
    rep = getattr(s, "ink_panel", None)
    if not rep:
        print("device reported no panel= — packing as %s" % knobs.panel)
        return True
    if rep == knobs.panel:
        return True
    if forced and forced != "auto":
        print("MISMATCH: device on %s reports panel=%s, --panel says %s.\n"
              "  Flash the matching env or drop --panel. Refusing to stream a "
              "%d-byte plane at it."
              % (s.port, rep, knobs.panel, knobs.geo.plane))
        return False
    if rep not in GEO:
        print("device reports panel=%s, which this path does not pack" % rep)
        return False
    print("device reports panel=%s (host had %s) — repacking" % (rep, knobs.panel))
    knobs.panel = rep
    knobs.geo = geo_for(rep)
    return True


def park_rim(plane):
    plane[0, :] = 0xFF
    plane[-1, :] = 0xFF
    plane[:, 0] |= 0x80
    plane[:, -1] |= 0x01
    return plane


def open_video(path):
    """Open a file. Do not pre-decode a frame — that doubled start cost on 720p."""
    backends = []
    if hasattr(cv2, "CAP_FFMPEG"):
        backends.append(cv2.CAP_FFMPEG)
    backends.append(cv2.CAP_ANY)
    for be in backends:
        cap = cv2.VideoCapture(path, be)
        if cap.isOpened():
            return cap
        cap.release()
    return None


def _src_max_edge(knobs):
    geo = getattr(knobs, "geo", None)
    if geo is None:
        return 480
    return min(960, max(geo.nw, geo.nh, geo.lw, geo.lh) * 2)


def downscale_bgr(bgr, knobs):
    """720p+ decode is wasted: the panel is 200..800 px. Shrink before PIL."""
    h, w = bgr.shape[:2]
    need = _src_max_edge(knobs)
    m = max(h, w)
    if m <= need:
        return bgr
    sc = need / float(m)
    return cv2.resize(bgr, (max(1, int(w * sc)), max(1, int(h * sc))),
                      interpolation=cv2.INTER_AREA)


def downscale_pil(im, knobs):
    need = _src_max_edge(knobs)
    w, h = im.size
    m = max(w, h)
    if m <= need:
        return im
    sc = need / float(m)
    return im.resize((max(1, int(w * sc)), max(1, int(h * sc))), Image.BILINEAR)


def _pil_rgb(im):
    """Flatten palette/alpha onto white so a GIF frame is a real RGB picture."""
    if im.mode == "RGB":
        return im.copy()
    if im.mode == "P":
        im = im.convert("RGBA") if "transparency" in im.info else im.convert("RGB")
    if im.mode == "RGBA":
        bg = Image.new("RGB", im.size, (255, 255, 255))
        bg.paste(im, mask=im.split()[-1])
        return bg
    return im.convert("RGB")


def load_still_or_gif(path):
    """Still image or animated GIF → (rgb_frames, delays_sec)."""
    im = Image.open(path)
    n = int(getattr(im, "n_frames", 1) or 1)
    frames, delays = [], []
    for i in range(n):
        im.seek(i)
        frames.append(_pil_rgb(im))
        delays.append(max(0.02, float(im.info.get("duration", 100) or 100) / 1000.0))
    if not frames:
        raise ValueError("no frames")
    return frames, delays


def _quiet_cv():
    os.environ["OPENCV_LOG_LEVEL"] = "ERROR"
    try:
        cv2.utils.logging.setLogLevel(cv2.utils.logging.LOG_LEVEL_ERROR)
    except Exception:
        pass


def _try_camera(index):
    # DSHOW first on Windows — MSMF can open a silent device
    # (LED off, frozen frames) while the real cam sits unused.
    # Do not force 1280x720: the laptop cam often fails the first grab if
    # you set size before it has delivered a frame.
    backends = []
    if hasattr(cv2, "CAP_DSHOW"):
        backends.append(cv2.CAP_DSHOW)
    backends.append(cv2.CAP_ANY)
    for attempt in range(3):
        for be in backends:
            cap = cv2.VideoCapture(int(index), be)
            if not cap.isOpened():
                cap.release()
                continue
            try:
                cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            except Exception:
                pass
            print("camera %s open, waiting for picture…" % index)
            frame = None
            t_end = time.time() + 8.0
            while time.time() < t_end:
                ok, f = cap.read()
                if not ok or f is None:
                    time.sleep(0.05)
                    continue
                frame = f
                if float(f.mean()) > 4.0:
                    break
            if frame is None:
                cap.release()
                time.sleep(0.4)
                continue
            # AE/AGC still ramps after the LED comes on.
            t_end = time.time() + 2.0
            while time.time() < t_end:
                ok, f = cap.read()
                if ok and f is not None:
                    frame = f
            print("camera %s ready  %dx%d  mean=%.1f" %
                  (index, frame.shape[1], frame.shape[0], float(frame.mean())))
            return cap
        time.sleep(0.5)
    return None


def open_camera(index):
    cap = _try_camera(index)
    if cap is not None:
        return cap
    print("camera %s produced no frames" % index)
    return None


def make_dpi_aware():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def virtual_screen():
    u = ctypes.windll.user32
    return (int(u.GetSystemMetrics(76)), int(u.GetSystemMetrics(77)),
            int(u.GetSystemMetrics(78)), int(u.GetSystemMetrics(79)))


def parse_region(text):
    """x,y,w,h in physical pixels. Also accepts x,y,w x h."""
    raw = text.replace("x", ",").replace("X", ",").replace(" ", "")
    parts = [int(p) for p in raw.split(",") if p != ""]
    if len(parts) != 4:
        raise ValueError("region must be x,y,w,h")
    x, y, w, h = parts
    if w < 16 or h < 16:
        raise ValueError("region too small (min 16x16)")
    return (x, y, x + w, y + h)


def clamp_bbox(box):
    vx, vy, vw, vh = virtual_screen()
    l, t, r, b = [int(v) for v in box]
    r = max(l + 16, r)
    b = max(t + 16, b)
    l = max(vx, min(l, vx + vw - 16))
    t = max(vy, min(t, vy + vh - 16))
    r = max(l + 16, min(r, vx + vw))
    b = max(t + 16, min(b, vy + vh))
    return (l, t, r, b)


def bbox_wh(box):
    l, t, r, b = box
    return r - l, b - t


def compose_source(im, knobs):
    geo = getattr(knobs, "geo", None) or GEO["xiao-426"]
    return compose_geo(im, knobs, geo)


def pick_screen_region(parent=None):
    """Drag a rectangle. Esc cancels. F = whole virtual desktop.

    Must be a Toplevel of the running app (not a second Tk() + nested
    mainloop), and must be *opaque*. A layered/alpha overlay on Windows
    delivers mouse-down and drag, then swallows mouse-up, so the box
    vanished and nothing was stored.
    """
    if tk is None:
        raise RuntimeError("tkinter missing; pass --region x,y,w,h instead")
    make_dpi_aware()
    vx, vy, vw, vh = virtual_screen()
    out = {"box": None}

    owned = parent is None
    if owned:
        win = tk.Tk()
    else:
        win = tk.Toplevel(parent)
        win.transient(parent)
    win.overrideredirect(True)
    win.geometry("%dx%d+%d+%d" % (vw, vh, vx, vy))
    try:
        win.attributes("-topmost", True)
    except tk.TclError:
        pass
    win.configure(bg="#101010")

    cv = tk.Canvas(win, width=vw, height=vh, highlightthickness=0,
                   bg="#101010", cursor="cross")
    cv.pack(fill="both", expand=True)

    # Frozen desktop behind the rubber-band, so the window can stay opaque
    # and still show what you are cropping.
    try:
        from PIL import ImageTk
        shot = ImageGrab.grab(bbox=(vx, vy, vx + vw, vy + vh), all_screens=True)
        if shot.mode != "RGB":
            shot = shot.convert("RGB")
        dim = (np.asarray(shot, dtype=np.uint16) * 38 // 100).astype(np.uint8)
        photo = ImageTk.PhotoImage(Image.fromarray(dim, "RGB"))
        win._photo = photo
        cv.create_image(0, 0, image=photo, anchor="nw")
    except Exception:
        pass

    cv.create_text(
        vw // 2, 28, fill="#f2f2f2", font=("Segoe UI", 16, "bold"),
        text="Drag a rectangle  ·  release to capture  ·  F = full desktop  ·  Esc = cancel")
    hint = cv.create_text(vw // 2, 56, fill="#dddd00", font=("Consolas", 14),
                          text="")
    rect = cv.create_rectangle(0, 0, 0, 0, outline="#ffee00", width=2)
    drag = {"x0": 0, "y0": 0, "on": False}

    def local_to_screen(x, y):
        return vx + int(x), vy + int(y)

    def show_size(x0, y0, x1, y1):
        cv.coords(rect, x0, y0, x1, y1)
        cv.itemconfigure(hint, text="%d × %d" % (abs(x1 - x0), abs(y1 - y0)))

    def finish(box):
        if box is not None:
            out["box"] = clamp_bbox(box)
        try:
            win.grab_release()
        except tk.TclError:
            pass
        win.destroy()

    def on_down(e):
        drag["x0"], drag["y0"], drag["on"] = int(e.x), int(e.y), True
        show_size(e.x, e.y, e.x, e.y)

    def on_move(e):
        if drag["on"]:
            show_size(drag["x0"], drag["y0"], e.x, e.y)

    def on_up(e):
        if not drag["on"]:
            return
        drag["on"] = False
        x0, y0 = drag["x0"], drag["y0"]
        x1, y1 = int(e.x), int(e.y)
        if abs(x1 - x0) < 16 or abs(y1 - y0) < 16:
            cv.itemconfigure(hint, text="too small — drag again or press F / Esc")
            return
        sl, st = local_to_screen(min(x0, x1), min(y0, y1))
        sr, sb = local_to_screen(max(x0, x1), max(y0, y1))
        finish((sl, st, sr, sb))

    def on_esc(_e):
        finish(None)

    def on_full(_e):
        finish((vx, vy, vx + vw, vy + vh))

    for w in (win, cv):
        w.bind("<ButtonPress-1>", on_down)
        w.bind("<B1-Motion>", on_move)
        w.bind("<ButtonRelease-1>", on_up)
        w.bind("<Escape>", on_esc)
        w.bind("<F>", on_full)
        w.bind("<f>", on_full)
    # The window must be MAPPED before it can be grabbed. Calling grab_set()
    # on an unmapped window raises "grab failed: window not viewable", and
    # swallowing that left the overlay with no grab at all -- it painted the
    # frozen desktop, took no clicks, and went away having stored nothing.
    # That is the "flashes up like snipping mode, then nothing" failure.
    win.update_idletasks()
    win.deiconify()
    win.lift()
    try:
        win.wait_visibility(win)
    except tk.TclError:
        pass
    win.focus_force()
    cv.focus_set()
    grabbed = False
    for _try in range(20):
        try:
            win.grab_set()
            grabbed = True
            break
        except tk.TclError:
            win.update()
            time.sleep(0.02)
    if not grabbed:
        # Keyboard and mouse bindings still work without an exclusive grab;
        # say so rather than failing silently the way this used to.
        print("region: no pointer grab (another window holds one) — "
              "the overlay still takes clicks")
    # A leaked grab is not a cosmetic failure: while one is held, no other
    # window on this interpreter can take one, and every ttk combobox popdown
    # in the app silently refuses to open. Release it whatever happens.
    try:
        if owned:
            win.mainloop()
        else:
            parent.wait_window(win)
    finally:
        try:
            win.grab_release()
        except Exception:
            pass
        try:
            if win.winfo_exists():
                win.destroy()
        except Exception:
            pass
    return out["box"]


def print_knobs(knobs):
    """Every live 0-255 control. User sets these; host does not retune."""
    g255 = int(round(128.0 * knobs.gamma))
    c255 = int(round(128.0 * knobs.contrast))
    print("0-255 knobs (128 = unity). Say the name and the number.")
    print("  name        now   0 ................ 255")
    print(f"  black      {int(knobs.black):3d}   crush floor          off")
    print(f"  white      {int(knobs.white):3d}   clip highlights      off")
    print(f"  gamma      {g255:3d}   lighter mids         darker mids")
    print(f"  contrast   {c255:3d}   flat                 punch")
    print(f"  highlights {int(knobs.highlights):3d}   crush brights        off")
    print(f"  knee       {int(knobs.knee):3d}   highlight start")
    print(f"  dither     {int(knobs.dither):3d}   gray steps 2/4/16/64  (d/D)")
    print(f"  bias       {int(knobs.bias):3d}   more black            more white")
    print(f"  trail        {knobs.trail:d}   0..12 re-drive history (t/T)")
    print(f"  chase        {knobs.chase:d}   0=off  1=every frame  2=every other")
    print(f"  drive        {knobs.drive:d}   1..12 waveform frames (not 0-255)")
    print("  vsh1       0x4B   +17 V  (0x41=+15 0x46=+16 0x4B=+17)")
    print("  vsl        0x3A   -17 V  (0x32=-15 0x36=-16 0x3A=-17)")
    print("  invert      off   do not flip the plane unless you say so")


class Knobs:
    def __init__(self, args):
        self.drive = args.drive
        self.volt_i = 2
        self.gamma = args.gamma
        self.contrast = args.contrast
        self.highlights = args.highlights
        self.knee = args.knee
        self.white = args.white
        self.black = args.black
        self.dither = args.dither
        self.bias = args.bias
        self.trail = 0
        self.chase = 0
        # Filled in from STATUS by confirm_panel(). None means "not asked yet",
        # and panel_fps() falls back to the port's boot defaults.
        self.fr = None
        self.spi_hz = None
        self.invert = args.invert
        self.mirror = not bool(args.no_mirror)
        self.fit = args.fit
        self.zoom = int(getattr(args, "zoom", 100) or 100)
        self.pad = int(getattr(args, "pad", 0) or 0)
        self.landscape = bool(args.landscape)
        self.test = bool(args.test)
        self.screen = bool(getattr(args, "screen", False))
        self.region = getattr(args, "region", None)
        if self.region == "":
            self.region = None
        self.panel = getattr(args, "panel", "auto")
        self.bb = int(getattr(args, "bb", 20))
        self.ww = int(getattr(args, "ww", 0) or 0)
        self.equalize_min = int(getattr(args, "equalize_min", 0) or 0)
        self.geo = geo_for("xiao-426")
        self.wire = 2
        # Set by the handshake when it consumed frame 0's ACK.
        self.ack_pending = False
        self.paused = False

    def line(self):
        geo = getattr(self, "geo", None)
        return (f"drive {self.drive} "
                f"(~{panel_fps(self.drive, geo, self.fr, self.spi_hz):.1f} fps)  "
                f"trail {self.trail}/{self.chase}  "
                f"dith {int(self.dither)}  "
                f"{VOLT_N[self.volt_i]}  g={self.gamma:.2f} "
                f"c={self.contrast:.2f} hi={self.highlights:.0f}  "
                f"{'INV' if self.invert else 'pos'} "
                f"{'mirror' if self.mirror else 'raw'} "
                f"{'wide' if self.landscape else 'portrait'}"
                f"{' PAUSE' if self.paused else ''}")


class Cam(threading.Thread):
    def __init__(self, src, knobs):
        super().__init__(daemon=True)
        self.src, self.knobs = src, knobs
        self.latest = None
        self.latest_im = None
        self.running = True
        self.grabs = 0
        self.error = None
        self.is_file = False
        self.nframes = 0
        self.src_fps = 0.0
        self._seek = None
        self._lock = threading.Lock()
        self._pos = 0

    def position(self):
        return self._pos, self.nframes

    def seek_frac(self, frac):
        if not self.is_file or self.nframes <= 0:
            return
        with self._lock:
            self._seek = int(max(0, min(self.nframes - 1, float(frac) * self.nframes)))

    def _emit(self, bgr):
        # Not every source is 3-channel BGR: a PNG sequence read through
        # VideoCapture comes back BGRA, and BGR2RGB throws on 4 channels.
        if bgr.ndim == 2:
            im = Image.fromarray(bgr)
        elif bgr.shape[2] == 4:
            im = Image.fromarray(cv2.cvtColor(bgr, cv2.COLOR_BGRA2RGB))
        else:
            im = Image.fromarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
        self._emit_pil(im)

    def _emit_pil(self, im):
        im = downscale_pil(im, self.knobs)
        self.latest_im = im
        self.latest = compose_source(im, self.knobs)
        self.grabs += 1

    def _run_image(self, path):
        try:
            frames, delays = load_still_or_gif(path)
        except Exception as e:
            self.error = "no image %s: %s" % (path, e)
            return
        self.is_file = True
        frames = [downscale_pil(f, self.knobs) for f in frames]
        self.nframes = len(frames)
        avg = sum(delays) / float(len(delays))
        self.src_fps = (1.0 / avg) if avg > 0 else 10.0
        n = len(frames)
        if n == 1:
            while self.running:
                self._pos = 0
                self._emit_pil(frames[0])
                t_end = time.time() + 0.05
                while self.running and time.time() < t_end:
                    if self.knobs.paused:
                        time.sleep(0.02)
                    else:
                        time.sleep(0.01)
            return
        origin = time.time()
        idx = 0
        while self.running:
            if self.knobs.paused:
                t_pause = time.time()
                while self.running and self.knobs.paused:
                    time.sleep(0.02)
                origin += time.time() - t_pause
                continue
            with self._lock:
                seek = self._seek
                self._seek = None
            if seek is not None:
                idx = max(0, min(n - 1, seek))
                origin = time.time()
            self._pos = idx
            self._emit_pil(frames[idx])
            due = origin + delays[idx]
            wait = due - time.time()
            if wait > 0.002:
                time.sleep(min(wait, delays[idx]))
            origin = time.time()
            idx = (idx + 1) % n

    def run(self):
        if isinstance(self.src, str) and not str(self.src).isdigit():
            ext = os.path.splitext(self.src)[1].lower()
            if ext in IMAGE_EXTS:
                self._run_image(self.src)
                return
            cap = open_video(self.src)
            if cap is None:
                self.error = f"no video {self.src}"
                return
            self.is_file = True
            self.src_fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
            if self.src_fps < 1.0:
                self.src_fps = 30.0
            # FRAME_COUNT on a long GOP file can scan the index. Do not block
            # the first picture on it: start playback, fill nframes after.
            self.nframes = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
        else:
            cap = open_camera(int(self.src))
            if cap is None:
                self.error = f"no camera {self.src}"
                return
            self.is_file = False
        try:
            if not self.is_file:
                while self.running:
                    ok, bgr = cap.read()
                    if not ok or bgr is None:
                        time.sleep(0.01)
                        continue
                    self._emit(downscale_bgr(bgr, self.knobs))
                return
            # File: wall-clock movie PTS at src_fps, but only *decode* about
            # as fast as the panel. 720p60 grab-skip was 2–3× slower than a
            # small file because every skipped source frame was still decoded.
            origin = time.time()
            base = 0
            cur = 0
            n = max(0, self.nframes)
            rate = self.src_fps
            geo = getattr(self.knobs, "geo", None)
            pf = panel_fps(int(getattr(self.knobs, "drive", DRIVE)), geo,
                           getattr(self.knobs, "fr", None),
                           getattr(self.knobs, "spi_hz", None))
            show_fps = min(rate, max(pf * 1.2, 12.0))
            while self.running:
                if self.knobs.paused:
                    t_pause = time.time()
                    while self.running and self.knobs.paused:
                        time.sleep(0.02)
                    origin += time.time() - t_pause
                    continue
                with self._lock:
                    seek = self._seek
                    self._seek = None
                if seek is not None:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, seek)
                    cur = seek
                    base = seek
                    origin = time.time()
                want = base + int((time.time() - origin) * rate)
                if n > 1 and want >= n:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    cur = 0
                    base = 0
                    origin = time.time()
                    want = 0
                if want != cur:
                    # Seek instead of grab-decoding every GOP frame we skip.
                    cap.set(cv2.CAP_PROP_POS_FRAMES, max(0, want))
                    cur = max(0, want)
                ok, bgr = cap.read()
                cur += 1
                if not ok or bgr is None:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    cur = 0
                    base = 0
                    origin = time.time()
                    time.sleep(0.01)
                    continue
                self._pos = cur
                self._emit(downscale_bgr(bgr, self.knobs))
                due = time.time() + (1.0 / show_fps)
                wait = due - time.time()
                if wait > 0.002:
                    time.sleep(min(wait, 0.08))
        finally:
            cap.release()


class ScreenGrab(threading.Thread):
    """Grab a free-form desktop rectangle. Same .latest native plane as Cam."""

    def __init__(self, bbox, knobs):
        super().__init__(daemon=True)
        self.knobs = knobs
        self._lock = threading.Lock()
        self.bbox = clamp_bbox(bbox)
        self.latest = None
        self.latest_im = None
        self.running = True
        self.grabs = 0
        self.error = None
        self.is_file = False
        self.nframes = 0
        self.src_fps = 0.0

    def position(self):
        return 0, 0

    def seek_frac(self, frac):
        pass

    def set_bbox(self, bbox):
        with self._lock:
            self.bbox = clamp_bbox(bbox)

    def run(self):
        make_dpi_aware()
        try:
            while self.running:
                with self._lock:
                    box = self.bbox
                try:
                    im = ImageGrab.grab(bbox=box, all_screens=True)
                except Exception as e:
                    self.error = "screen grab failed: %s" % e
                    time.sleep(0.05)
                    continue
                if im.mode != "RGB":
                    im = im.convert("RGB")
                self.latest_im = im
                self.latest = compose_source(im, self.knobs)
                self.grabs += 1
                time.sleep(0.01)
        except Exception as e:
            self.error = "screen grab failed: %s" % e


def open_source(src, knobs):
    if knobs.screen:
        box = knobs.region
        if box is None:
            box = pick_screen_region()
        if box is None:
            fail("screen region cancelled")
        knobs.region = box
        print("screen %d,%d %dx%d" % (box[0], box[1], box[2] - box[0], box[3] - box[1]))
        grab = ScreenGrab(box, knobs)
        grab.start()
        return grab
    cam = Cam(src, knobs)
    cam.start()
    return cam


def open_serial(port):
    last = None
    for _ in range(10):
        s = None
        try:
            s = serial.Serial()
            s.port, s.baudrate = port, BAUD
            s.timeout, s.write_timeout = 4, 8
            s.dtr = s.rts = False
            s.open()
            time.sleep(0.12)
            try:
                s.write(CAN)
                s.flush()
            except Exception:
                pass
            wait_bytes(s, b"STREAM done", 0.2)
            # STATUS is the one authoritative statement of which env is
            # flashed. Drain a window and search the WHOLE blob rather than
            # stopping at the first newline after the word STATUS. That reply
            # is multi-line now (eopt, plus a FAST block when a custom waveform
            # is armed), and a stale byte still in flight could end a line
            # before `panel=` was reached -- the host then fell back to
            # xiao-426 and enter_stream refused the mismatch. A clean failure,
            # but a needless one, and it hit every other run.
            # Bounded: a firmware that logs continuously (the CrowPanel demo
            # prints EAT / RESULT lines every few frames) never goes quiet, and
            # an unbounded wait-for-quiet hung Connect forever. Short reads,
            # stop on quiet OR once the STATUS line is in, hard cap regardless.
            raw = b""
            s.timeout = 0.05
            for _attempt in range(2):
                s.reset_input_buffer()
                s.write(b"status\n")
                s.flush()
                t_start = quiet = time.time()
                while time.time() - quiet < 0.15 and time.time() - t_start < 1.5:
                    chunk = s.read(256)
                    if chunk:
                        raw += chunk
                        quiet = time.time()
                    if b"STATUS" in raw and raw.endswith(b"\n"):
                        break
                if panel_from_text(raw) or b"STATUS" in raw:
                    break
            s.timeout = 4
            s.ink_panel = panel_from_text(raw)
            s.ink_fr, s.ink_spi_hz = timing_from_text(raw)
            s.ink_status = raw.decode("utf-8", "replace").strip()
            s.reset_input_buffer()
            return s
        except Exception as e:
            last = e
            # A failure AFTER open() used to leave the port open in this process,
            # so every later attempt -- this loop's retries and every button --
            # got "Access is denied" until the app was closed.
            if s is not None:
                try:
                    s.close()
                except Exception:
                    pass
            time.sleep(0.25)
    raise last


def wait_bytes(s, needle, timeout, to_nl=False, abort=None):
    """Read one byte at a time. Stop at needle, or at the newline after it.

    to_nl=True is required for STREAM ready: the host must see `wire=2` on
    that same line. Stopping at `STREAM ready` spoke wire=1 to a wire=2 chip
    → one static frame, then hang.
    """
    buf = b""
    t0 = time.time()
    found = False
    while time.time() - t0 < timeout:
        if abort is not None and abort():
            return False, buf
        b = s.read(1)
        if not b:
            continue
        buf += b
        if needle in buf:
            found = True
            if not to_nl or b == b"\n":
                return True, buf
    return found, buf


def stream_handshake(s, timeout=8.0):
    """Scan for `STREAM ready`, then stop at the FIRST ACK byte.

    Do not wait for the banner's newline. The firmware runs TinyUSB CDC; if its
    TX timeout is 0 a full FIFO silently drops bytes, and the newline is exactly
    what goes missing. wait_bytes(to_nl=True) then reads straight past the
    device's ACK and eats it — the device sits out its 4 s header timeout and
    aborts with `STREAM done 0 frames`, which reads on the host as a bad frame,
    a bad encode, or a dead panel. It is none of those.

    Firmware now caps that timeout at 20 ms so the newline should survive, but
    this stays newline-independent: it costs nothing and the failure it prevents
    is unreadable from the outside.

    Returns (wire|None, ack_pending, banner_text). ack_pending=True means the
    device's first ACK is already in hand, so frame 0 must not wait for another.
    """
    buf = bytearray()
    ready = False
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = s.read(1)
        if not b:
            continue
        if ready and b == TOKEN:
            return pack_wire_banner(buf), True, buf.decode("utf-8", "replace")
        buf += b
        if not ready and b"STREAM ready" in buf:
            ready = True
        elif ready and b == b"\n":
            return pack_wire_banner(buf), False, buf.decode("utf-8", "replace")
    if ready:
        # Banner seen but neither its newline nor an ACK arrived intact. The
        # device is in the stream loop regardless, so proceed and let the first
        # wait_token() find the ACK.
        return pack_wire_banner(buf), False, buf.decode("utf-8", "replace")
    return None, False, buf.decode("utf-8", "replace")


def wait_token(s, abort=None, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if abort is not None and abort():
            return False
        b = s.read(1)
        if b == TOKEN:
            return True
        if not b:
            continue
    return False


def send_blob(s, blob, abort=None):
    for off in range(0, len(blob), CHUNK):
        if not wait_token(s, abort=abort):
            return False
        try:
            s.write(blob[off:off + CHUNK])
        except serial.SerialTimeoutException:
            return False
    return True


def send_frame(s, bmp, lit, wire, abort=None, have_token=False):
    """have_token: the ACK for this frame was already consumed (handshake)."""
    if wire >= 2:
        blob = pack_wire2(bmp, lit)
        if not have_token and not wait_token(s, abort=abort):
            return False
        try:
            s.write(blob)
        except serial.SerialTimeoutException:
            return False
        return True
    return send_blob(s, bmp, abort=abort) and send_blob(s, lit, abort=abort)


def enter_stream(s, knobs, do_white=True):
    s.reset_input_buffer()
    s.write(f"volts {VOLTS[knobs.volt_i]}\n".encode())
    wait_bytes(s, b"OK volts", 2.0)
    s.write(f"drive {knobs.drive}\n".encode())
    wait_bytes(s, b"OK drive", 2.0)
    s.write(("trail %d\n" % knobs.trail).encode())
    wait_bytes(s, b"OK trail", 2.0)
    s.write(("chase %d\n" % knobs.chase).encode())
    wait_bytes(s, b"OK chase", 2.0)
    if knobs.panel == "xiao-154":
        # Only the custom-waveform build has this, and its reply is the
        # multi-line FAST block rather than an OK line.
        s.write(("bb %d\n" % int(getattr(knobs, "bb", 0))).encode())
        wait_bytes(s, b"FAST", 2.0)
        s.write(("ww %d\n" % int(getattr(knobs, "ww", 0))).encode())
        wait_bytes(s, b"FAST", 2.0)
    if do_white:
        # 1.54" `white` is a black/white SEQ_FULL deghost (~4 s) and stream
        # entry only needs a known base. `white1` is one refresh. 4.26" has
        # `white` as seed, not white1.
        if knobs.panel == "xiao-154":
            s.write(b"white1\n")
            print("seed white…")
            wait_bytes(s, b"OK white1", 8.0)
        else:
            s.write(b"white\n")
            print("seed white…")
            wait_bytes(s, b"OK white", 8.0)
    s.write(b"stream\n")
    wire, ack_pending, banner = stream_handshake(s, 8.0)
    if wire is None:
        print("no STREAM ready:\n" + banner[-300:])
        return False
    knobs.wire = wire
    knobs.ack_pending = ack_pending
    # The banner also names the panel. Last chance to catch a plane-size
    # mismatch before frames start going out.
    rep = panel_from_text(banner)
    if rep and rep != knobs.panel:
        print("MISMATCH: stream banner says panel=%s, host packs %s"
              % (rep, knobs.panel))
        return False
    # Short reads so Stop/CAN do not sit on a 4 s serial timeout.
    s.timeout = 0.05
    print("stream %s" % banner.strip()[-180:])
    return True


def leave_stream(s):
    # After an ACK the device is waiting for a chunk. Lone CAN aborts.
    # If we are between ACKs, just stop; 4 s timeout also ends it.
    try:
        s.timeout = 4
    except Exception:
        pass
    try:
        s.write(CAN)
        s.flush()
    except Exception:
        pass
    wait_bytes(s, b"STREAM done", 6.0)


# Vendor full-refresh busy, measured. Each polarity is one drive cycle
# (rails up, full LUT, rails down). 1.54" OTP 0xF7 = 2132 ms; 4.26" Mode 1
# = 540 ms; 5.79" vendor path ~336 ms.
EQUALIZE_MINS = (1, 5, 15, 30, 45, 60)
FULL_REFRESH_MS = {
    "xiao-154": 2132,
    "xiao-426": 540,
    "crow-579": 336,
    "auto": 2132,
}


def equalize_drive_count(minutes, panel="auto"):
    ms = FULL_REFRESH_MS.get(panel, 2132)
    return max(1, int(round(float(minutes) * 60.0 * 1000.0 / ms)))


def equalize_label(minutes, panel="auto"):
    n = equalize_drive_count(minutes, panel)
    return "%d min  (~%s drives)" % (int(minutes), format(n, ","))


def equalize_panel(port, minutes, panel="auto", log=None, should_stop=None,
                   on_status=None):
    """Cycle solid black/white on the vendor full sequence (rails up/down).

    Drive is set to 12 first. Loop until `minutes` elapse or should_stop.
    Each polarity counts as one drive. Ends on white.
    """
    def say(msg):
        if log is not None:
            log(msg)
        print(msg)

    def stopped():
        return should_stop is not None and should_stop()

    minutes = int(minutes)
    if minutes not in EQUALIZE_MINS:
        minutes = min(EQUALIZE_MINS, key=lambda m: abs(m - minutes))
    panel = detect_panel(port, panel)
    expect = equalize_drive_count(minutes, panel)
    s = serial.Serial()
    s.port, s.baudrate = port, BAUD
    s.timeout, s.write_timeout = 0.2, 8
    s.dtr = s.rts = False
    try:
        s.open()
    except Exception as e:
        return "cannot open %s: %s" % (port, e)
    drives = 0
    t_end = time.time() + minutes * 60.0
    try:
        time.sleep(0.15)
        try:
            s.write(CAN)
            s.flush()
        except Exception:
            pass
        wait_bytes(s, b"STREAM done", 2.0, abort=stopped)
        s.reset_input_buffer()
        s.write(b"status\n")
        _ok, raw = wait_bytes(s, b"STATUS", 3.0, abort=stopped)
        reported = panel_from_text(raw)
        if reported:
            panel = reported
            expect = equalize_drive_count(minutes, panel)
        s.write(b"drive 12\n")
        wait_bytes(s, b"OK drive", 3.0, abort=stopped)
        say("equalize: drive 12, vendor full rails, %d min (~%s drives) %s"
            % (minutes, format(expect, ","), panel))

        def one(cmd, needle, timeout):
            if stopped():
                return False
            s.reset_input_buffer()
            s.write((cmd + "\n").encode())
            s.flush()
            ok, _raw = wait_bytes(s, needle, timeout, abort=stopped)
            return ok

        while time.time() < t_end and not stopped():
            left = max(0, int(t_end - time.time()))
            if on_status:
                on_status("equalize %d:%02d left  %s drives  mode 12 full"
                          % (left // 60, left % 60, format(drives, ",")))
            if panel == "xiao-154":
                if not one("black", b"OK black", 8.0):
                    break
                drives += 1
                if stopped() or time.time() >= t_end:
                    break
                if not one("white1", b"OK white1", 8.0):
                    break
                drives += 1
            elif panel == "xiao-426":
                if not one("deghost 1", b"DEGHOST done", 12.0):
                    break
                drives += 2
            else:
                if not one("flush 1", b"OK flush", 20.0):
                    break
                drives += 2
        if panel == "xiao-154":
            one("white1", b"OK white1", 8.0)
        elif panel == "xiao-426":
            one("white", b"OK white", 8.0)
        else:
            # `flush` leaves the 5.79" on its bench card; end on white like
            # the other two.
            one("white", b"OK white", 8.0)
        say("equalize done %s drives (est %s)" % (format(drives, ","),
                                                  format(expect, ",")))
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass


def park_panel(port, color="white", cycles=8, panel="auto", log=None):
    """Stop whatever is running, deghost, hold white or black.

    Clearing flashes are the vendor full-refresh path — a differential
    update never lifts idle residue. Drive is set to 12 first (strongest
    custom table) so a later stream re-entry is not sitting on a weak
    mode. 5–10 cycles; default 8.
    """
    def say(msg):
        if log is not None:
            log(msg)
        print(msg)

    color = "black" if str(color).lower().startswith("b") else "white"
    cycles = max(5, min(10, int(cycles)))
    panel = detect_panel(port, panel)
    s = serial.Serial()
    s.port, s.baudrate = port, BAUD
    s.timeout, s.write_timeout = 4, 8
    s.dtr = s.rts = False
    try:
        s.open()
    except Exception as e:
        return "cannot open %s: %s" % (port, e)
    try:
        time.sleep(0.3)
        try:
            s.write(CAN)
            s.flush()
        except Exception:
            pass
        wait_bytes(s, b"STREAM done", 2.0)
        s.reset_input_buffer()
        s.write(b"status\n")
        _ok, raw = wait_bytes(s, b"STATUS", 3.0)
        reported = panel_from_text(raw)
        if reported:
            panel = reported
        s.write(b"drive 12\n")
        wait_bytes(s, b"OK drive", 3.0)
        say("park: drive 12, %d flashes, hold %s (%s)" % (cycles, color, panel))
        if panel == "xiao-154":
            s.write(("scrub %d\n" % cycles).encode())
            ok, raw = wait_bytes(s, b"OK scrub", 90.0)
            if not ok:
                return "scrub timed out:\n" + raw.decode("utf-8", "replace")[-200:]
        elif panel == "xiao-426":
            n = min(cycles, 8)
            s.write(("deghost %d\n" % n).encode())
            ok, raw = wait_bytes(s, b"DEGHOST done", 90.0)
            if not ok:
                return "deghost timed out:\n" + raw.decode("utf-8", "replace")[-200:]
        else:
            s.write(("flush %d\n" % cycles).encode())
            ok, raw = wait_bytes(s, b"OK flush", 90.0)
            if not ok:
                return "flush timed out:\n" + raw.decode("utf-8", "replace")[-200:]
        if panel == "crow-579":
            # `flush` ends on the bench card, not on white, so the hold is an
            # explicit white or black (both added to the 5.79" firmware).
            s.write(("%s\n" % color).encode())
            ok, raw = wait_bytes(s, ("OK %s" % color).encode(), 30.0)
            if not ok:
                return "%s timed out:\n%s" % (
                    color, raw.decode("utf-8", "replace")[-200:])
            say("parked %s" % color)
            return None
        if color == "black":
            s.write(b"black\n")
            ok, raw = wait_bytes(s, b"OK black", 30.0)
            if not ok:
                return "black timed out:\n" + raw.decode("utf-8", "replace")[-200:]
        elif panel == "xiao-426":
            s.write(b"white\n")
            wait_bytes(s, b"OK white", 30.0)
        say("parked %s" % color)
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass


def port_hwid(port):
    try:
        ports = serial.tools.list_ports.comports()
    except Exception:
        return ""
    want = port.upper()
    for p in ports:
        if (p.device or "").upper() == want:
            return (p.hwid or "").upper()
    return ""


def detect_panel(port, forced="auto"):
    hwid = port_hwid(port)
    usb = "crow-579" if "1A86:" in hwid else ("xiao-426" if "303A:" in hwid else "")
    if forced and forced != "auto":
        xiao = usb == "xiao-426" and forced in ("xiao-426", "xiao-154")
        if usb and forced != usb and not xiao:
            sys.exit(
                "COM %s is %s, not %s. Plug the matching USB-C "
                "(Xiao=303A TinyUSB, CrowPanel=1A86 CH340)."
                % (port, usb, forced)
            )
        return forced
    return usb or "xiao-426"


# Anticlockwise pre-rotation of the source before it is framed into the
# CrowPanel's 272 x 792 portrait canvas, which reaches the glass turned 90
# clockwise. 90 showed the picture a quarter turn short as the panel is held;
# 0 is 90 clockwise from that, judged on glass 2026-10-12.
CROW_ROT = 0


def open_serial_crow(port):
    s = serial.Serial()
    s.port, s.baudrate = port, 115200
    s.timeout, s.write_timeout = 4, 8
    s.dtr = s.rts = False
    s.open()
    try:
        time.sleep(0.4)
        s.reset_input_buffer()
    except Exception:
        s.close()  # never leave the port held by a half-opened handle
        raise
    return s


CROW_STREAM_BAUD = 2_000_000
HUD_RE = re.compile(rb"\bhud=(\d+)")


def crow_reenter(s, knobs):
    """`stream2` (RLE change map), falling back to `stream` on older firmware.

    Reads the banner off the line and switches both ends to 2 Mbaud. Sets
    knobs.crow_rle and knobs.crow_hud (white bar height the firmware draws
    over the top of the picture, 0 if it does not say).
    """
    for cmd in (b"stream2\n", b"stream\n"):
        s.baudrate = 115200
        time.sleep(0.1)
        s.reset_input_buffer()
        s.write(cmd)
        buf = b""
        line = None
        t0 = time.time()
        while time.time() - t0 < 6:
            buf += s.read(256)
            i = buf.find(b"STREAM ready")
            if i >= 0 and b"\n" in buf[i:]:
                line = buf[i:buf.index(b"\n", i)]
                break
            if b"ERR unknown" in buf:
                break
        if line is None:
            continue
        knobs.crow_rle = b"map=rle" in line
        m = HUD_RE.search(line)
        knobs.crow_hud = int(m.group(1)) if m else 0
        print(line.decode("utf-8", "replace").strip())
        time.sleep(0.05)
        s.baudrate = CROW_STREAM_BAUD
        time.sleep(0.15)
        # Drop the rate-change garbage and any acks sent while still slow; the
        # device repeats its ack every 150 ms until data flows.
        s.reset_input_buffer()
        return True
    print("device did not enter stream mode")
    return False


def enter_stream_crow(s, knobs):
    # The banner is read 256 bytes at a time. With the 4 s timeout from
    # open_serial_crow, that read sat 4 s on a ~60 byte banner while the
    # board, already at 2 Mbaud, timed out waiting for data and fell back to
    # 115200: "STREAM done 0 frames", then a host at 2 Mbaud that never sees an
    # ACK. Short reads here.
    s.timeout = 0.05
    s.baudrate = 115200
    time.sleep(0.05)
    s.reset_input_buffer()
    s.write(b"old 1\n")
    wait_bytes(s, b"OK old", 2.0)
    s.write(b"settle 1\n")
    wait_bytes(s, b"OK settle", 2.0)
    # Volts: the same ±15/16/17 V choice as the other ports, as this port's
    # vsh1/vsl registers (it has no `volts` command). Never sent before, so
    # the Volts buttons did nothing here. Firmware refuses anything above 0x4B/0x3A.
    vsh1, vsl = VOLTS[knobs.volt_i].split()
    s.write(("vsh1 0x%s\n" % vsh1).encode())
    wait_bytes(s, b"OK vsh1", 2.0)
    s.write(("vsl 0x%s\n" % vsl).encode())
    wait_bytes(s, b"OK vsl", 2.0)
    s.write(("drive %d\n" % knobs.drive).encode())
    print("crow drive %d (GC + sweep)…" % knobs.drive)
    ok, raw = wait_bytes(s, b"OK drive", 20.0)
    if not ok:
        print("no OK drive:\n" + raw.decode("utf-8", "replace")[-300:])
        return False
    return crow_reenter(s, knobs)


def leave_stream_crow(s):
    try:
        s.write(CAN)
        s.flush()
    except Exception:
        pass
    try:
        s.baudrate = 115200
    except Exception:
        pass
    time.sleep(0.2)
    wait_bytes(s, b"STREAM done", 6.0)


def crow_cmd(s, line, needle, timeout=10.0):
    """One command at 115200 outside a stream. Returns (ok, reply text)."""
    s.baudrate = 115200
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    s.flush()
    ok, raw = wait_bytes(s, needle, timeout, to_nl=True)
    return ok, raw.decode("utf-8", "replace")


def encode_one(src, prev, prev_g, prev_bit, knobs, geo, tone_lut, thresh,
               fps_plate, skip_hold=False):
    g = tone_lut[np.clip(src, 0, 255)]
    if fps_plate is not None:
        g = g.copy()
        blit_fps_plate(g, fps_plate, portrait=geo.portrait and not knobs.landscape)
    bit = g > thresh
    bit = np.where(g <= knobs.black, False, bit)
    bit = np.where(g >= knobs.white, True, bit)
    if (not skip_hold) and prev_g is not None:
        hold = np.abs(g.astype(np.int16) - prev_g.astype(np.int16)) < HOLD
        bit = np.where(hold, prev_bit, bit)
    if knobs.invert:
        bit = ~bit
    cur = np.packbits(bit, axis=1)
    park_rim(cur)
    bmp, lit = encode_delta(cur, prev)
    return cur, g, bit, bmp, lit


def xiao_stream_body(s, knobs, cam, ctl, poll=None):
    """Encode the next plane while USB waits for ACK. Returns (why, sent, t0)."""
    geo = knobs.geo
    thresh = build_threshold(knobs.dither, knobs.bias, geo.nh, geo.nw)
    tone_lut = build_lut(knobs.gamma, knobs.contrast, knobs.white, knobs.black)
    prev = np.full((geo.nh, geo.stride), 0xFF, dtype=np.uint8)
    park_rim(prev)
    state = {
        "thresh": thresh,
        "tone_lut": tone_lut,
        "prev": prev,
        "prev_g": None,
        "prev_bit": None,
        "fps_plate": make_fps_plate(0.0) if geo.nh >= 400 else None,
        "frozen": None,
    }
    enc_stop = threading.Event()
    out_q = queue.Queue(maxsize=1)
    sent = 0
    t0 = time.time()
    win_t0 = t0
    win_n = 0
    fps_built_at = t0
    enc_bytes = 0

    def should_stop():
        return (not getattr(ctl, "running", True)) or enc_stop.is_set()

    def worker():
        while not enc_stop.is_set():
            src = test_card(geo) if knobs.test else (None if cam is None else cam.latest)
            if src is None:
                time.sleep(0.004)
                continue
            if knobs.paused:
                if state["frozen"] is None:
                    state["frozen"] = np.array(src, copy=True)
                src = state["frozen"]
            else:
                state["frozen"] = None
            src = np.asarray(src)
            if src.shape[0] != geo.nh or src.shape[1] != geo.nw:
                time.sleep(0.004)
                continue
            src = np.array(src, copy=True)
            skip_hold = bool(state.get("skip_hold"))
            if skip_hold:
                state["skip_hold"] = False
            cur, g, bit, bmp, lit = encode_one(
                src, state["prev"], state["prev_g"], state["prev_bit"],
                knobs, geo, state["tone_lut"], state["thresh"],
                state["fps_plate"], skip_hold=skip_hold)
            packed = (cur, g, bit, bmp, lit)
            while not enc_stop.is_set():
                try:
                    out_q.put(packed, timeout=0.05)
                    state["prev"] = cur
                    state["prev_g"] = g
                    state["prev_bit"] = bit
                    break
                except queue.Full:
                    continue

    th = threading.Thread(target=worker, name="eink-encode", daemon=True)
    th.start()
    why = "stop"
    try:
        while not should_stop():
            if poll:
                act = poll()
                if act == "quit":
                    why = "quit"
                    break
                if act in ("rearm", "white"):
                    why = act
                    break
                if act == "tone":
                    state["thresh"] = build_threshold(
                        knobs.dither, knobs.bias, geo.nh, geo.nw)
                    state["tone_lut"] = build_lut(
                        knobs.gamma, knobs.contrast, knobs.white, knobs.black)
                    state["skip_hold"] = True
                    state["prev_g"] = None
                    state["prev_bit"] = None
                    state["frozen"] = None
            if getattr(ctl, "_hold_color", None):
                why = "hold"
                break
            if getattr(ctl, "_white", False) or getattr(ctl, "_rearm", False):
                why = "white" if ctl._white else "rearm"
                ctl._white = ctl._rearm = False
                break
            if getattr(ctl, "_cmds", None):
                # Arbitrary text cannot go down the wire mid-stream: the device
                # is reading raw frame bytes and would eat it as payload. So
                # leave, run the commands, and re-enter.
                why = "cmd"
                break
            if getattr(ctl, "_tone", False):
                ctl._tone = False
                state["thresh"] = build_threshold(
                    knobs.dither, knobs.bias, geo.nh, geo.nw)
                state["tone_lut"] = build_lut(
                    knobs.gamma, knobs.contrast, knobs.white, knobs.black)
                # HOLD was swallowing still/GIF slider moves (gray moved < 12).
                # Next frame is a real delta of the new tone, no reconnect.
                state["skip_hold"] = True
                state["prev_g"] = None
                state["prev_bit"] = None
                state["frozen"] = None
            try:
                _cur, _g, _bit, bmp, lit = out_q.get(timeout=0.05)
            except queue.Empty:
                continue
            held = bool(getattr(knobs, "ack_pending", False))
            if not send_frame(s, bmp, lit, knobs.wire, abort=should_stop,
                              have_token=held):
                # send_frame also returns False when the abort callback fires,
                # so an ordinary Stop used to be reported as a dead device.
                why = "stop" if should_stop() else "dead"
                break
            if held:
                knobs.ack_pending = False
            sent += 1
            win_n += 1
            enc_bytes += len(bmp) + len(lit)
            now = time.time()
            if now - win_t0 >= 1.0:
                fps = win_n / (now - win_t0)
                ctl.fps = fps
                win_t0 = now
                win_n = 0
                ctl.status = (
                    "streaming  %.1f fps  drive %d  trail %d/%d  dith %d  wire %d"
                    % (fps, knobs.drive, knobs.trail, knobs.chase,
                       knobs.dither, knobs.wire))
            if state["fps_plate"] is not None and now - fps_built_at >= 10.0:
                span = now - fps_built_at
                shown = (win_n / span) if span > 0.05 else getattr(ctl, "fps", 0.0)
                state["fps_plate"] = make_fps_plate(ctl.fps if ctl.fps else shown)
                fps_built_at = now
            if sent % 5 == 0 and poll is not None:
                el = max(now - t0, 0.001)
                avg = enc_bytes / max(sent, 1)
                print("\r%d f  %5.2f fps  drive %d  %s  %d B/f  " %
                      (sent, sent / el, knobs.drive, geo.name, int(avg)),
                      end="", flush=True)
        else:
            why = "stop"
    finally:
        enc_stop.set()
        try:
            out_q.get_nowait()
        except queue.Empty:
            pass
    return why, sent, t0


def run_crow_loop(s, knobs, cam, should_stop=None, poll=None, ctl=None):
    """Stream to the CrowPanel until something needs the port. Returns
    (why, sent, t0); why is stop / quit / dead / rearm / white / hold / cmd.

    `ctl` is the LiveStream (Ink Flow): its rearm, white, hold and queued
    console commands end the loop the same way they end xiao_stream_body, and
    the caller leaves the stream, acts, and re-enters. This loop used to watch
    only console keys, so every one of those buttons did nothing on this panel
    and the status line never left "opening source". `poll` is the console
    key reader, for the command-line host only.
    """
    from play_video import send_chunked

    out_q = queue.Queue(maxsize=1)
    enc_stop = threading.Event()
    enc = CrowEncoder(knobs, cam, int(getattr(knobs, "crow_hud", 0) or 0),
                      bool(getattr(knobs, "crow_rle", False)))

    def worker():
        # Encode frame N+1 while frame N is on the wire, as the XIAO path does.
        # Done in series it added 6-17 ms to every frame, and its jitter.
        while not enc_stop.is_set():
            item = enc.next_frame()
            if item is None:
                time.sleep(0.01)
                continue
            while not enc_stop.is_set():
                try:
                    out_q.put(item, timeout=0.05)
                    break
                except queue.Full:
                    continue

    th = threading.Thread(target=worker, name="crow-encode", daemon=True)
    th.start()
    sent = 0
    t0 = time.time()
    win_t0, win_n = t0, 0
    why = "stop"
    s.timeout = 0.05
    try:
        while True:
            if should_stop is not None and should_stop():
                break
            act = poll() if poll is not None else None
            if act == "quit":
                why = "quit"
                break
            if act in ("rearm", "white"):
                why = act
                break
            if act == "tone":
                enc.skip_hold = True
            if ctl is not None:
                if getattr(ctl, "_hold_color", None):
                    why = "hold"
                    break
                if getattr(ctl, "_white", False) or getattr(ctl, "_rearm", False):
                    why = "white" if ctl._white else "rearm"
                    ctl._white = ctl._rearm = False
                    break
                if getattr(ctl, "_cmds", None):
                    why = "cmd"
                    break
                if getattr(ctl, "_tone", False):
                    # A slider move must show at once, not be held back by
                    # the hold threshold. Tables rebuild from the knobs.
                    ctl._tone = False
                    enc.skip_hold = True
            try:
                head, tail = out_q.get(timeout=0.05)
            except queue.Empty:
                continue
            if not send_chunked(s, head) or not send_chunked(s, tail):
                why = "stop" if (should_stop is not None and should_stop()) else "dead"
                break
            sent += 1
            win_n += 1
            now = time.time()
            if ctl is not None and now - win_t0 >= 1.0:
                ctl.fps = win_n / (now - win_t0)
                win_t0, win_n = now, 0
                ctl.status = ("streaming  %.1f fps  drive %d  dith %d  crow-579"
                              % (ctl.fps, knobs.drive, int(knobs.dither)))
            if sent % 5 == 0 and poll is not None:
                el = max(now - t0, 0.001)
                print("\r%d f  %5.2f fps  drive %d  %s  " %
                      (sent, sent / el, knobs.drive, "crow-579"),
                      end="", flush=True)
    finally:
        enc_stop.set()
    return why, sent, t0


class CrowEncoder:
    """Source picture -> one packed CrowPanel frame, ready for the wire.

    What the XIAO path already did and this panel lacked, measured on Big Buck
    Bunny over the CH340's ~200 kB/s (median / p95 / worst wire time per
    frame): 28 / 52 / 153 ms before, about 13 / 40 / 106 ms after.

    - HOLD: a pixel keeps its last bit unless its grey moved by HOLD or more,
      so sensor and codec noise stop flipping dither cells every frame.
    - The change map goes RLE when the firmware takes `stream2` (map=rle).
    - No keyframe every 60 frames (a full 30.6 KB plane, ~150 ms of wire: a
      regular hitch). A rolling band of ROLL_BYTES is re-sent each frame
      instead, which heals a desynchronised byte within ROLL_FRAMES frames at
      ~2 ms of wire.
    - The top `hud` rows stay white and the picture is framed below them, so
      no video plays under the firmware's HUD bar.
    """

    ROLL_FRAMES = 64

    def __init__(self, knobs, cam, hud, rle):
        import encode_video as crow_enc
        self.ce = crow_enc
        self.knobs, self.cam = knobs, cam
        self.hud = max(0, min(int(hud), 200))
        self.rle = rle
        self.xmap = crow_enc.portrait_x_map()
        self.area = Geo("crow-area", 272, 792 - self.hud, 272, 792 - self.hud, False)
        self.canvas = np.full((792, 272), 255, dtype=np.uint8)
        self.prev = np.full(crow_enc.PLANE_BYTES, 0xFF, dtype=np.uint8)
        self.prev_g = None
        self.prev_bit = None
        self.skip_hold = False
        self.tone_key = None
        self.lut = self.thresh = None
        self.frozen = None
        self.roll = 0
        self.roll_bytes = -(-crow_enc.PLANE_BYTES // self.ROLL_FRAMES)

    def _source(self):
        k = self.knobs
        if k.test:
            im = Image.new("L", (640, 360), 40)
            dr = ImageDraw.Draw(im)
            dr.rectangle((20, 20, 620, 80), fill=255)
            dr.ellipse((220, 100, 420, 300), fill=255)
            return im
        im = self.cam.latest_im if self.cam is not None else None
        if im is None:
            return None
        if k.paused:
            if self.frozen is None:
                self.frozen = im.copy()
            return self.frozen
        self.frozen = None
        return im

    def next_frame(self):
        """(head, tail) bytes for send_chunked, or None when no picture yet."""
        k = self.knobs
        im = self._source()
        if im is None:
            return None
        if CROW_ROT:
            im = im.rotate(CROW_ROT, expand=True)
        # Framing into the area below the HUD bar; the bar rows stay white.
        self.canvas[self.hud:, :] = compose_geo(im, k, self.area)
        key = (k.gamma, k.contrast, k.white, k.black, int(k.dither), k.bias,
               bool(k.invert))
        if key != self.tone_key:
            self.tone_key = key
            lut = build_lut(k.gamma, k.contrast, k.white, k.black)
            self.lut = (255 - lut) if k.invert else lut
            self.thresh = build_threshold(k.dither, k.bias, 792, 272)
            self.skip_hold = True
        g = self.lut[self.canvas]
        bit = g >= self.thresh                       # True = white
        if self.skip_hold:
            self.skip_hold = False
        elif self.prev_g is not None:
            hold = np.abs(g.astype(np.int16) - self.prev_g.astype(np.int16)) < HOLD
            bit = np.where(hold, self.prev_bit, bit)
        self.prev_g, self.prev_bit = g, bit
        # Portrait grid -> native column-major plane, as image_to_fb_rot does.
        nat = np.zeros((272, 800), dtype=bool)      # True = black
        nat[:, self.xmap] = ~bit.T
        fb = np.ascontiguousarray(np.packbits(~nat, axis=1).T).reshape(-1)
        diff = fb != self.prev
        a = self.roll * self.roll_bytes
        diff[a:a + self.roll_bytes] = True
        self.roll = (self.roll + 1) % self.ROLL_FRAMES
        self.prev = fb
        bmp = np.packbits(diff)
        pay = fb[diff]
        ck = int(np.bitwise_xor.reduce(bmp)) ^ (
            int(np.bitwise_xor.reduce(pay)) if pay.size else 0)
        bmp_b = bmp.tobytes()
        tail = pay.tobytes() + bytes([ck])
        if not self.rle:
            return bmp_b, tail
        rle = rle_occupancy(bmp_b)
        if len(rle) < len(bmp_b):
            flags, occ = 1, rle
        else:
            flags, occ = 0, bmp_b
        n = len(occ)
        return bytes((flags, n & 255, (n >> 8) & 255)) + occ, tail


def handle_key(ch, knobs):
    if ch in (b"q", b"Q", b"\x1b"):
        return "quit"
    if ch == b"p":
        knobs.paused = not knobs.paused
        print("\n" + knobs.line())
        return None
    if ch == b"i":
        knobs.invert = not knobs.invert
        print("\n" + knobs.line())
        return None
    if ch == b"m":
        knobs.mirror = not knobs.mirror
        print("\n" + knobs.line())
        return None
    if ch == b"s":
        return "repick"
    if ch == b"t":
        knobs.trail = max(0, knobs.trail - 1)
        print("\ntrail %d" % knobs.trail)
        return "rearm"
    if ch == b"T":
        knobs.trail = min(12, knobs.trail + 1)
        print("\ntrail %d" % knobs.trail)
        return "rearm"
    if ch == b"e":
        knobs.chase = max(0, knobs.chase - 1)
        print("\nchase every %d" % knobs.chase)
        return "rearm"
    if ch == b"E":
        knobs.chase = min(8, knobs.chase + 1)
        print("\nchase every %d" % knobs.chase)
        return "rearm"
    if ch == b"d":
        knobs.dither = dither_step(knobs.dither, -1)
        print("\ngray levels %d" % knobs.dither)
        return "tone"
    if ch == b"D":
        knobs.dither = dither_step(knobs.dither, 1)
        print("\ngray levels %d" % knobs.dither)
        return "tone"
    if ch == b"b":
        knobs.bias = max(1, knobs.bias - 8)
        print("\nbias %d (lower=more black)" % knobs.bias)
        return "tone"
    if ch == b"B":
        knobs.bias = min(254, knobs.bias + 8)
        print("\nbias %d (higher=more white)" % knobs.bias)
        return "tone"
    if ch == b"w":
        return "white"
    if ch == b"[":
        knobs.drive = max(1, knobs.drive - 1)
        return "rearm"
    if ch == b"]":
        knobs.drive = min(12, knobs.drive + 1)
        return "rearm"
    if ch == b"0":
        knobs.drive = 10
        return "rearm"
    if ch in b"123456789":
        knobs.drive = int(ch)
        return "rearm"
    if ch == b",":
        knobs.volt_i = max(0, knobs.volt_i - 1)
        return "rearm"
    if ch == b".":
        knobs.volt_i = min(2, knobs.volt_i + 1)
        return "rearm"
    if ch == b"g":
        knobs.gamma = max(0.5, knobs.gamma - 0.05)
    elif ch == b"G":
        knobs.gamma = min(3.0, knobs.gamma + 0.05)
    elif ch == b"c":
        knobs.contrast = max(0.4, knobs.contrast - 0.05)
    elif ch == b"C":
        knobs.contrast = min(3.0, knobs.contrast + 0.05)
    elif ch == b"h":
        knobs.highlights = max(knobs.knee, knobs.highlights - 5)
    elif ch == b"H":
        knobs.highlights = min(255.0, knobs.highlights + 5)
    elif ch == b"k":
        knobs.knee = max(20.0, knobs.knee - 5)
    elif ch == b"K":
        knobs.knee = min(250.0, knobs.knee + 5)
    else:
        return None
    print("\n" + knobs.line())
    return "tone"


def poll_keys(knobs):
    if not msvcrt:
        return None
    act = None
    while msvcrt.kbhit():
        ch = msvcrt.getch()
        if ch in (b"\x00", b"\xe0"):
            msvcrt.getch()
            continue
        act = handle_key(ch, knobs) or act
    return act


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--panel", default="auto",
                    choices=("auto", "xiao-426", "xiao-154", "crow-579"),
                    help="auto = USB id (303A Xiao, 1A86 CrowPanel CH340). "
                         "xiao-154 = Watchy 200x200")
    ap.add_argument("--camera", type=int, default=1,
                    help="cv2 camera index; --list to enumerate")
    ap.add_argument("--source", default="",
                    help="video / gif / image path; loops; overrides --camera")
    ap.add_argument("--drive", type=int, default=DRIVE,
                    help="Mode 2 drive_tp (waveform frames)")
    ap.add_argument("--test", action="store_true",
                    help="send a high-contrast test card instead of the camera")
    ap.add_argument("--gamma", type=float, default=GAMMA)
    ap.add_argument("--contrast", type=float, default=CONTRAST)
    ap.add_argument("--white", type=int, default=WHITE_PT)
    ap.add_argument("--black", type=int, default=BLACK_PT)
    ap.add_argument("--dither", type=int, default=DITHER,
                    help="16=4x4 cluster, 64=8x8 cluster")
    ap.add_argument("--bias", type=int, default=BIAS,
                    help="128=50/50, lower=more black cells")
    ap.add_argument("--highlights", type=float, default=255)
    ap.add_argument("--knee", type=float, default=150.0)
    ap.add_argument("--fit", action="store_true",
                    help="letterbox instead of centre-cropping to fill")
    ap.add_argument("--zoom", type=int, default=100,
                    help="percent, 100 = none; crops a centred window first")
    ap.add_argument("--pad", type=int, default=0,
                    help="letterbox bars: 0 black, 255 white, -1 extend (edge pixels)")
    ap.add_argument("--landscape", action="store_true")
    ap.add_argument("--no-mirror", action="store_true")
    ap.add_argument("--invert", dest="invert", action="store_true",
                    default=INVERT)
    ap.add_argument("--no-invert", dest="invert", action="store_false")
    ap.add_argument("--screen", action="store_true",
                    help="desktop rectangle instead of camera/file")
    ap.add_argument("--region", default="",
                    help="screen region x,y,w,h (skips the drag overlay)")
    args = ap.parse_args()
    _quiet_cv()

    knobs = Knobs(args)
    if args.region:
        knobs.region = parse_region(args.region)
        knobs.screen = True
    panel = detect_panel(args.port, args.panel)
    knobs.panel = panel
    knobs.geo = geo_for(panel)
    if panel == "crow-579":
        knobs.mirror = False
        if args.drive == DRIVE:
            knobs.drive = 4
        if abs(args.contrast - CONTRAST) < 1e-6:
            knobs.contrast = 1.8
        print("panel crow-579  (wavelab stream, 792x272, 2 Mbaud)")
    else:
        print("panel %s  (%dx%d, wire auto)" %
              (panel, knobs.geo.nw, knobs.geo.nh))

    cam = None
    if not knobs.test:
        src = args.source if args.source else args.camera
        # Mirroring is a selfie affordance. On a file it just flips the movie.
        if args.source:
            knobs.mirror = False
        cam = open_source(src, knobs)
        t0 = time.time()
        while cam.error is None and time.time() - t0 < 20:
            ready = cam.latest_im if panel == "crow-579" else cam.latest
            if ready is not None:
                break
            time.sleep(0.02)
        if cam.error:
            fail(cam.error)
        if panel == "crow-579":
            if cam.latest_im is None:
                fail("camera produced no frames")
            print("source %s" % cam.latest_im.size)
        else:
            if cam.latest is None:
                fail("camera produced no frames")
            src0 = cam.latest
            print("camera gray mean=%.1f min=%d max=%d" %
                  (float(src0.mean()), int(src0.min()), int(src0.max())))

    print(knobs.line())
    print_knobs(knobs)
    print("keys: [ ] drive  , . volts  g/c tone  i invert  p pause  q quit")

    if panel == "crow-579":
        s = open_serial_crow(args.port)
        sent = 0
        t0 = time.time()
        try:
            if not enter_stream_crow(s, knobs):
                sys.exit(1)
            while True:
                why, sent, t0 = run_crow_loop(
                    s, knobs, cam, poll=lambda: poll_keys(knobs))
                if why not in ("rearm", "white"):
                    if why == "dead":
                        print("\ndevice stopped asking")
                    break
                # Stream entry seeds white, so White is a plain re-entry here.
                leave_stream_crow(s)
                if not enter_stream_crow(s, knobs):
                    break
                print(knobs.line())
        except KeyboardInterrupt:
            pass
        finally:
            if cam is not None:
                cam.running = False
            try:
                leave_stream_crow(s)
            except Exception:
                pass
            el = max(time.time() - t0, 0.001)
            print("\n%d frames  %.2f fps" % (sent, sent / el))
            s.close()
        return

    class _Ctl:
        running = True
        _rearm = False
        _white = False
        _tone = False
        fps = 0.0
        status = ""

    ctl = _Ctl()
    s = open_serial(args.port)
    if getattr(s, "ink_status", ""):
        print("device %s" % s.ink_status[-160:])
    if not confirm_panel(s, knobs, args.panel):
        s.close()
        sys.exit(1)
    sent = 0
    t0 = time.time()
    try:
        if not enter_stream(s, knobs, do_white=True):
            s.close()
            sys.exit(1)
        print("wire=%d  %s" % (knobs.wire, knobs.geo.name))
        while ctl.running:
            why, sent, t0 = xiao_stream_body(
                s, knobs, cam, ctl, poll=lambda: poll_keys(knobs))
            if why == "quit" or why == "dead" or why == "stop":
                if why == "dead":
                    print("\ndevice stopped asking")
                break
            if why in ("rearm", "white"):
                leave_stream(s)
                if not enter_stream(s, knobs, do_white=(why == "white")):
                    break
                print(knobs.line())
                continue
    except KeyboardInterrupt:
        pass
    finally:
        ctl.running = False
        if cam is not None:
            cam.running = False
        try:
            leave_stream(s)
        except Exception:
            pass
        el = max(time.time() - t0, 0.001)
        print(f"\n{sent} frames  {sent / el:.2f} fps")
        s.close()


class LiveStream:
    """GUI-driven Xiao/Crow stream. No console keys. Mutate .knobs live."""

    def __init__(self, port, source, knobs, panel="auto"):
        self.port = port
        self.source = source
        self.knobs = knobs
        self.panel = panel
        self.cam = None
        self.fps = 0.0
        self.status = "idle"
        self.error = None
        self.running = False
        self._th = None
        self._rearm = False
        self._white = False
        self._tone = False
        self._hold_color = None
        self._cmds = []
        # Ring of device text for a UI to display. Bounded so a long session
        # cannot grow without limit.
        self.conlog = collections.deque(maxlen=400)

    def send(self, line):
        """Queue a serial command. Runs between frames when streaming.

        While streaming the port is owned by the frame loop, so the command is
        applied by leaving the stream, sending it, and re-entering. When idle it
        goes straight out.
        """
        line = (line or "").strip()
        if not line:
            return
        if self.running:
            self._cmds.append(line)
            self.conlog.append("> " + line + "   (queued)")
        else:
            self._send_offline(line)

    def _send_offline(self, line):
        self.conlog.append("> " + line)
        try:
            s = serial.Serial()
            s.port, s.baudrate = self.port, BAUD
            s.timeout, s.write_timeout = 0.3, 4
            s.dtr = s.rts = False
            s.open()
        except Exception as e:
            self.conlog.append("  cannot open %s: %s" % (self.port, e))
            return
        try:
            time.sleep(0.3)
            s.reset_input_buffer()
            s.write((line + "\n").encode())
            s.flush()
            self._drain(s)
        finally:
            s.close()

    def _drain(self, s, quiet_for=0.45, cap=12.0):
        """Read until the device stops talking. A `white` can take seconds."""
        buf = b""
        t0 = last = time.time()
        while time.time() - last < quiet_for and time.time() - t0 < cap:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                last = time.time()
        for ln in buf.decode("utf-8", "replace").splitlines():
            if ln.strip():
                self.conlog.append("  " + ln.rstrip())

    def _run_cmds(self, s):
        pending, self._cmds = self._cmds, []
        for line in pending:
            self.conlog.append("> " + line)
            try:
                s.reset_input_buffer()
                s.write((line + "\n").encode())
                s.flush()
                self._drain(s)
            except Exception as e:
                self.conlog.append("  send failed: %s" % e)

    def start(self):
        if self.running:
            return
        self.error = None
        self.running = True
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()

    def stop(self):
        self.running = False
        if self.cam is not None:
            self.cam.running = False

    def request_rearm(self):
        self._rearm = True

    def request_white(self):
        self._white = True

    def request_hold(self, color):
        """While paused: leave the stream, full-clear to white or black at drive 12."""
        self._hold_color = "black" if str(color).lower().startswith("b") else "white"

    def park(self, color="white", cycles=8):
        """Leave the stream, deghost, hold white or black. Blocks."""
        self.stop()
        t0 = time.time()
        while self._th is not None and self._th.is_alive() and time.time() - t0 < 6:
            time.sleep(0.05)
        self.running = False
        return park_panel(self.port, color=color, cycles=cycles,
                          panel=self.panel, log=self.conlog.append)

    def request_tone(self):
        self._tone = True

    def seek_frac(self, frac):
        if self.cam is not None:
            self.cam.seek_frac(frac)

    def position(self):
        if self.cam is None:
            return 0, 0
        return self.cam.position()

    def _run(self):
        _quiet_cv()
        knobs = self.knobs
        try:
            panel = detect_panel(self.port, self.panel)
            knobs.panel = panel
            knobs.geo = geo_for(panel)
            if int(getattr(knobs, "equalize_min", 0) or 0) > 0:
                self.status = "equalize…"
                err = equalize_panel(
                    self.port, knobs.equalize_min, panel=self.panel,
                    log=self.conlog.append,
                    should_stop=lambda: not self.running,
                    on_status=lambda m: setattr(self, "status", m),
                )
                if err:
                    self.error = err
                    self.status = err
                elif self.running:
                    self.status = "equalize done"
                else:
                    self.status = "equalize stopped"
                return
            if panel == "crow-579":
                knobs.mirror = False
            if (isinstance(self.source, str) and self.source
                    and not str(self.source).isdigit()):
                knobs.mirror = False   # file source: do not flip the movie
            self.status = "opening source…"
            if knobs.screen:
                self.cam = open_source(self.source, knobs)
            else:
                self.cam = Cam(self.source, knobs)
                self.cam.start()
            t0 = time.time()
            while self.running and self.cam.error is None and time.time() - t0 < 20:
                if self.cam.latest is not None or self.cam.latest_im is not None:
                    break
                time.sleep(0.05)
            if self.cam.error:
                self.error = self.cam.error
                self.status = self.cam.error
                return
            if self.cam.latest is None and self.cam.latest_im is None:
                self.error = "no frames"
                self.status = "no frames"
                return
            if panel == "crow-579":
                self._run_crow(knobs)
                return
            self._run_xiao(knobs)
        # SystemExit too: detect_panel() calls sys.exit on a forced-panel
        # mismatch, and in this thread that ended the stream with no message.
        except (Exception, SystemExit) as e:
            self.error = str(e) or type(e).__name__
            self.status = self.error
        finally:
            self.running = False
            if self.cam is not None:
                self.cam.running = False
            st = self.status or ""
            if (not st or st == "opening source…" or st.startswith("streaming")
                    or st.startswith("entering") or st.startswith("re-entering")):
                self.status = "stopped"

    def _run_xiao(self, knobs):
        self.status = "opening serial…"
        s = open_serial(self.port)
        try:
            if not confirm_panel(s, knobs, self.panel):
                self.error = "panel mismatch: device says %s, host packs %s" % (
                    getattr(s, "ink_panel", "?"), knobs.panel)
                self.status = self.error
                return
            self.status = "entering stream…"
            if not enter_stream(s, knobs, do_white=True):
                self.error = "no STREAM ready"
                self.status = "no STREAM ready"
                return
            self.status = "streaming wire=%d" % knobs.wire
            while self.running:
                why, _sent, _t0 = xiao_stream_body(s, knobs, self.cam, self)
                if why in ("dead", "stop", "quit"):
                    if why == "dead":
                        self.status = "device stopped asking"
                    break
                if why == "cmd":
                    leave_stream(s)
                    self._run_cmds(s)
                    if not enter_stream(s, knobs, do_white=False):
                        self.error = "re-enter failed after command"
                        break
                    continue
                if why in ("rearm", "white"):
                    leave_stream(s)
                    if not enter_stream(s, knobs, do_white=(why == "white")):
                        self.error = "rearm failed"
                        break
                    continue
                if why == "hold":
                    # The eraser: 8 deghost cycles at drive 12, then hold. That
                    # many is what it takes to lift what the stream left on the
                    # glass, which is the point of the button.
                    color = self._hold_color or "white"
                    self._hold_color = None
                    leave_stream(s)
                    try:
                        s.close()
                    except Exception:
                        pass
                    self.status = "holding %s (drive 12 full clear)…" % color
                    err = park_panel(self.port, color=color, cycles=8,
                                     panel=knobs.panel, log=self.conlog.append)
                    self.status = err or ("paused, held %s" % color)
                    while self.running and knobs.paused:
                        nxt = self._hold_color
                        if nxt:
                            self._hold_color = None
                            self.status = "holding %s (drive 12 full clear)…" % nxt
                            err = park_panel(self.port, color=nxt, cycles=8,
                                             panel=knobs.panel,
                                             log=self.conlog.append)
                            self.status = err or ("paused, held %s" % nxt)
                        time.sleep(0.05)
                    if not self.running:
                        break
                    self.status = "restoring…"
                    s = open_serial(self.port)
                    if not confirm_panel(s, knobs, self.panel):
                        self.error = "panel mismatch on restore"
                        self.status = self.error
                        break
                    if not enter_stream(s, knobs, do_white=True):
                        self.error = "restore failed"
                        self.status = self.error
                        break
                    self.status = "streaming (restored)"
                    continue
        finally:
            try:
                leave_stream(s)
            except Exception:
                pass
            s.close()

    def _crow_hold(self, s, color):
        """White out / Black out on the CrowPanel while streaming: the same
        eraser as the XIAO panels, 8 deghost cycles at drive 12 (`flush 8`),
        then hold. Runs on the open port; park_panel would reopen it."""
        self.status = "holding %s (drive 12 full clear)…" % color
        steps = (("drive 12", b"OK drive", 30.0), ("flush 8", b"OK flush", 90.0),
                 (color, ("OK %s" % color).encode(), 30.0))
        for line, needle, limit in steps:
            ok, raw = crow_cmd(s, line, needle, limit)
            for ln in raw.splitlines():
                if ln.strip():
                    self.conlog.append("  " + ln.rstrip())
            if not ok:
                self.status = "%s: no reply from the panel" % line
                return
        self.status = "paused, held %s" % color

    def _run_crow(self, knobs):
        """Same button contract as _run_xiao: leave the stream, act, re-enter."""
        s = open_serial_crow(self.port)
        in_stream = False
        try:
            self.status = "entering stream…"
            if not enter_stream_crow(s, knobs):
                self.error = "no STREAM ready (crow)"
                self.status = self.error
                return
            in_stream = True
            self.status = "streaming"
            while self.running:
                why, sent, t0 = run_crow_loop(
                    s, knobs, self.cam, should_stop=lambda: not self.running,
                    ctl=self)
                if why in ("dead", "stop", "quit"):
                    if why == "dead":
                        self.status = "device stopped asking"
                    break
                leave_stream_crow(s)
                in_stream = False
                if why == "cmd":
                    self._run_cmds(s)
                elif why == "hold":
                    color = self._hold_color or "white"
                    self._hold_color = None
                    self._crow_hold(s, color)
                    while self.running and knobs.paused:
                        nxt = self._hold_color
                        if nxt:
                            self._hold_color = None
                            self._crow_hold(s, nxt)
                        time.sleep(0.05)
                    if not self.running:
                        break
                # rearm and white: re-entry alone does it. Entry re-sends volts
                # and drive, and stream_run seeds the panel white.
                self.status = "re-entering stream…"
                if not enter_stream_crow(s, knobs):
                    self.error = "re-enter failed (crow)"
                    self.status = self.error
                    break
                in_stream = True
                self.status = "streaming"
        finally:
            if in_stream:
                try:
                    leave_stream_crow(s)
                except Exception:
                    pass
            s.close()


if __name__ == "__main__":
    main()
