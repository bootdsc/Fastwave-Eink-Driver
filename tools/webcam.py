"""fastwave host — live webcam / clip to e-ink.

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

CROW_TOOLS = r"..\..\ssd1683-crowpanel-579\tools"
if CROW_TOOLS not in sys.path:
    sys.path.insert(0, CROW_TOOLS)
LAST_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "last_stream.log")


def fail(msg):
    print(msg)
    try:
        with open(LAST_LOG, "w", encoding="utf-8") as fh:
            fh.write(msg + "\n")
    except Exception:
        pass
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
DITHER = 16
# 128 = 50/50 at mid-grey. Lower = more black cells (the 4x4 was too white).
BIAS = 64
DRIVE = 6
# Reuse last bit if gray moved less than this — stops cam noise flipping
# every pixel every frame.
HOLD = 12
INVERT = False
# 115200 is the working CDC rate on this XIAO. 230400 also works. 921600 does not.
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


def panel_fps(drive, geo=None):
    # drive+settle waveform + one-plane SPI (~18 ms at 40 MHz) + USB
    if geo is not None and geo.name == "xiao-154":
        # Measured law, custom waveform: busy = frames x 8.063 ms at FR 0x05,
        # frames = drive + settle(1), plus ~4 ms for one 5000-byte plane at
        # 20 MHz. Only valid with `fast on`; the OTP partial is a flat 363 ms.
        return 1000.0 / ((drive + 1) * 8.063 + 4.0)
    return 1000.0 / ((drive + 1) * 9.92 + 20.0)


def fit_wh(im, tw, th, letterbox=False, pad=0):
    if im.mode != "L":
        im = im.convert("L")
    if im.width == tw and im.height == th:
        return np.asarray(im)
    if letterbox:
        scale = min(tw / im.width, th / im.height)
        nw = max(1, int(im.width * scale + 0.5))
        nh = max(1, int(im.height * scale + 0.5))
        im = im.resize((nw, nh), Image.BILINEAR)
        canvas = Image.new("L", (tw, th), pad)
        canvas.paste(im, ((tw - nw) // 2, (th - nh) // 2))
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
    pad   bar colour, 0 black / 255 white. On a mostly-white scene black bars
          read as part of the picture, which is why it is a choice.

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
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        return None
    ok, f = cap.read()
    if not ok or f is None:
        cap.release()
        return None
    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
    return cap


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


def pick_screen_region():
    """Drag a rectangle on a dim overlay. Esc cancels. F = whole virtual desktop."""
    if tk is None:
        raise RuntimeError("tkinter missing; pass --region x,y,w,h instead")
    make_dpi_aware()
    vx, vy, vw, vh = virtual_screen()
    out = {"box": None}

    root = tk.Tk()
    root.overrideredirect(True)
    root.geometry("%dx%d+%d+%d" % (vw, vh, vx, vy))
    root.attributes("-topmost", True)
    root.configure(bg="black")
    root.attributes("-alpha", 0.32)
    root.focus_force()

    cv = tk.Canvas(root, width=vw, height=vh, highlightthickness=0,
                   bg="#101010", cursor="cross")
    cv.pack(fill="both", expand=True)
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
        w, h = abs(x1 - x0), abs(y1 - y0)
        cv.coords(rect, x0, y0, x1, y1)
        cv.itemconfigure(hint, text="%d × %d" % (w, h))

    def finish(box):
        if box is not None:
            out["box"] = clamp_bbox(box)
        root.destroy()

    def on_down(e):
        drag["x0"], drag["y0"], drag["on"] = e.x, e.y, True
        show_size(e.x, e.y, e.x, e.y)

    def on_move(e):
        if drag["on"]:
            show_size(drag["x0"], drag["y0"], e.x, e.y)

    def on_up(e):
        if not drag["on"]:
            return
        drag["on"] = False
        x0, y0 = drag["x0"], drag["y0"]
        x1, y1 = e.x, e.y
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

    cv.bind("<ButtonPress-1>", on_down)
    cv.bind("<B1-Motion>", on_move)
    cv.bind("<ButtonRelease-1>", on_up)
    root.bind("<Escape>", on_esc)
    root.bind("<F>", on_full)
    root.bind("<f>", on_full)
    cv.bind("<Escape>", on_esc)
    cv.bind("<F>", on_full)
    cv.bind("<f>", on_full)
    root.grab_set()
    cv.focus_set()
    root.mainloop()
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
        self.trail = 7
        self.chase = 2
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
        self.bb = int(getattr(args, "bb", 0) or 0)
        self.ww = int(getattr(args, "ww", 0) or 0)
        self.geo = geo_for("xiao-426")
        self.wire = 2
        # Set by the handshake when it consumed frame 0's ACK.
        self.ack_pending = False
        self.paused = False

    def line(self):
        geo = getattr(self, "geo", None)
        return (f"drive {self.drive} (~{panel_fps(self.drive, geo):.1f} fps)  "
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
        self.latest_im = im
        self.latest = compose_source(im, self.knobs)
        self.grabs += 1

    def run(self):
        if isinstance(self.src, str) and not str(self.src).isdigit():
            cap = open_video(self.src)
            if cap is None:
                self.error = f"no video {self.src}"
                return
            self.is_file = True
            self.nframes = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
            self.src_fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
            if self.src_fps < 1.0:
                self.src_fps = 30.0
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
                    self._emit(bgr)
                return
            # File: wall-clock PTS. Panel fps only samples; movie length stays true.
            origin = time.time()
            base = 0
            cur = 0
            n = max(1, self.nframes)
            rate = self.src_fps
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
                if want >= n:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    cur = 0
                    base = 0
                    origin = time.time()
                    want = 0
                if want < cur or want > cur + 45:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, want)
                    cur = want
                else:
                    while cur < want:
                        if not cap.grab():
                            break
                        cur += 1
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
                self._emit(bgr)
                due = origin + (cur - base) / rate
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
        try:
            s = serial.Serial()
            s.port, s.baudrate = port, BAUD
            s.timeout, s.write_timeout = 4, 8
            s.dtr = s.rts = False
            s.open()
            time.sleep(0.35)
            try:
                s.write(CAN)
                s.flush()
            except Exception:
                pass
            wait_bytes(s, b"STREAM done", 0.4)
            # STATUS is the one authoritative statement of which env is
            # flashed. Drain a window and search the WHOLE blob rather than
            # stopping at the first newline after the word STATUS. That reply
            # is multi-line now (eopt, plus a FAST block when a custom waveform
            # is armed), and a stale byte still in flight could end a line
            # before `panel=` was reached -- the host then fell back to
            # xiao-426 and enter_stream refused the mismatch. A clean failure,
            # but a needless one, and it hit every other run.
            raw = b""
            for _attempt in range(3):
                s.reset_input_buffer()
                s.write(b"status\n")
                s.flush()
                quiet = time.time()
                while time.time() - quiet < 0.35:
                    chunk = s.read(256)
                    if chunk:
                        raw += chunk
                        quiet = time.time()
                if panel_from_text(raw):
                    break
            s.ink_panel = panel_from_text(raw)
            s.ink_status = raw.decode("utf-8", "replace").strip()
            s.reset_input_buffer()
            return s
        except Exception as e:
            last = e
            time.sleep(0.5)
    raise last


def wait_bytes(s, needle, timeout, to_nl=False):
    """Read one byte at a time. Stop at needle, or at the newline after it.

    to_nl=True is required for STREAM ready: the host must see `wire=2` on
    that same line. Stopping at `STREAM ready` spoke wire=1 to a wire=2 chip
    → one static frame, then hang.
    """
    buf = b""
    t0 = time.time()
    found = False
    while time.time() - t0 < timeout:
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
        s.write(b"white\n")
        print("deghost (black/white cycles)…")
        wait_bytes(s, b"OK white", 30.0)
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


def open_serial_crow(port):
    s = serial.Serial()
    s.port, s.baudrate = port, 115200
    s.timeout, s.write_timeout = 4, 8
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.4)
    s.reset_input_buffer()
    return s


def enter_stream_crow(s, knobs):
    from play_video import reenter_stream
    s.baudrate = 115200
    time.sleep(0.05)
    s.reset_input_buffer()
    s.write(b"old 1\n")
    wait_bytes(s, b"OK old", 2.0)
    s.write(b"settle 1\n")
    time.sleep(0.4)
    s.read(512)
    s.write(("drive %d\n" % knobs.drive).encode())
    print("crow drive %d (GC + sweep)…" % knobs.drive)
    ok, raw = wait_bytes(s, b"OK drive", 20.0)
    if not ok:
        print("no OK drive:\n" + raw.decode("utf-8", "replace")[-300:])
        return False
    return reenter_stream(s)


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


def encode_one(src, prev, prev_g, prev_bit, knobs, geo, tone_lut, thresh, fps_plate):
    g = tone_lut[np.clip(src, 0, 255)]
    if fps_plate is not None:
        g = g.copy()
        blit_fps_plate(g, fps_plate, portrait=geo.portrait and not knobs.landscape)
    bit = g > thresh
    bit = np.where(g <= knobs.black, False, bit)
    bit = np.where(g >= knobs.white, True, bit)
    if prev_g is not None:
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
            cur, g, bit, bmp, lit = encode_one(
                src, state["prev"], state["prev_g"], state["prev_bit"],
                knobs, geo, state["tone_lut"], state["thresh"],
                state["fps_plate"])
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


def run_crow_loop(s, knobs, cam, should_stop=None):
    import encode_video as crow_enc
    from play_video import send_chunked

    lut = crow_enc.build_lut(knobs.gamma, knobs.contrast)
    thresh = crow_enc.build_threshold(
        crow_enc.PORTRAIT_H, crow_enc.PORTRAIT_W, "cluster8")
    xmap = crow_enc.portrait_x_map()
    prev = np.full(crow_enc.PLANE_BYTES, 0xFF, dtype=np.uint8)
    sent = 0
    t0 = time.time()
    frozen = None
    enc_bytes = 0
    s.timeout = 0.05
    while True:
        if should_stop is not None and should_stop():
            break
        act = poll_keys(knobs)
        if act == "quit":
            break
        if act in ("rearm", "white"):
            leave_stream_crow(s)
            if not enter_stream_crow(s, knobs):
                break
            prev = np.full(crow_enc.PLANE_BYTES, 0xFF, dtype=np.uint8)
            sent = 0
            t0 = time.time()
            enc_bytes = 0
            print(knobs.line())
            continue
        if knobs.test:
            im = Image.new("L", (640, 360), 40)
            dr = ImageDraw.Draw(im)
            dr.rectangle((20, 20, 620, 80), fill=255)
            dr.ellipse((220, 100, 420, 300), fill=255)
        else:
            im = cam.latest_im if cam is not None else None
        if im is None:
            time.sleep(0.01)
            continue
        if knobs.paused:
            if frozen is None:
                frozen = im.copy()
            im = frozen
        else:
            frozen = None
        fb = crow_enc.image_to_fb_rot(im, lut, thresh, xmap, 90)
        bmp, pay = crow_enc.wire_frame(fb, prev, keyframe=(sent % 60 == 0))
        if not send_chunked(s, bmp) or not send_chunked(s, pay):
            print("\ndevice stopped asking")
            break
        prev = fb
        sent += 1
        enc_bytes += len(bmp) + len(pay)
        now = time.time()
        el = max(now - t0, 0.001)
        if sent % 5 == 0:
            avg = enc_bytes / max(sent, 1)
            print("\r%d f  %5.2f fps  drive %d  %s  " %
                  (sent, sent / el, knobs.drive, "crow-579"),
                  end="", flush=True)
    return sent, t0


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
                    help="auto = USB id (303A Xiao 4.26, 1A86 CrowPanel CH340). "
                         "xiao-154 is packing only until that env is flashed")
    ap.add_argument("--camera", type=int, default=1,
                    help="cv2 camera index; --list to enumerate")
    ap.add_argument("--source", default="",
                    help="video file path; loops; overrides --camera")
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
                    help="letterbox bar level, 0 black / 255 white")
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
            sent, t0 = run_crow_loop(s, knobs, cam)
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
            if panel == "crow-579":
                knobs.mirror = False
            if isinstance(self.source, str) and self.source                     and not str(self.source).isdigit():
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
        except Exception as e:
            self.error = str(e)
            self.status = str(e)
        finally:
            self.running = False
            if self.cam is not None:
                self.cam.running = False
            if not self.status or self.status == "opening source…":
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
        finally:
            try:
                leave_stream(s)
            except Exception:
                pass
            s.close()

    def _run_crow(self, knobs):
        s = open_serial_crow(self.port)
        try:
            if not enter_stream_crow(s, knobs):
                self.error = "no STREAM ready (crow)"
                self.status = self.error
                return
            sent, t0 = run_crow_loop(
                s, knobs, self.cam, should_stop=lambda: not self.running)
            el = max(time.time() - t0, 0.001)
            self.fps = sent / el
        finally:
            try:
                leave_stream_crow(s)
            except Exception:
                pass
            s.close()


if __name__ == "__main__":
    main()
