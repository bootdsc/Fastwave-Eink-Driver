#!/usr/bin/env python3
"""Capture the glass with a USB camera and report numbers, not adjectives.

  python tools\\panelshot.py out.png            # capture, crop, save, measure
  python tools\\panelshot.py --calibrate         # re-measure the active area

Why this exists: every waveform judgement in this project used to be "look at
the photo", one photo at a time. A contrast number lets a sweep be read as a
curve, which is the difference between guessing at a drive count and measuring
the impulse floor.

Resolve the camera BY NAME, not by index. Indices renumber when anything is
plugged in, and a virtual-camera driver will happily hand you a splash screen.
Set PANELCAP_CAM to your device's name.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import time

import numpy as np
from PIL import Image, ImageOps

CAM = os.environ.get("PANELCAP_CAM", "USB Camera")
# Ask for less than the sensor's maximum: many UVC devices silently fail the
# largest mode and hand back nothing.
SIZE = os.environ.get("PANELCAP_SIZE", "1920x1080")
WARM = int(os.environ.get("PANELCAP_WARM", "40"))
# The ACTIVE AREA, calibrated by differencing a solid-white panel against a
# solid-black one: the bounding box of what actually changed. Measured
# 2026-08-20 as (411,206)-(800,559), 389x353, aspect 1.102 (the panel sits at a
# slight angle to the camera).
#
# Do NOT eyeball this. A hand-picked crop that included the white bezel and the
# card's dark border line scored contrast 93-100 for EVERY drive count from 1 to
# 24, because p90 was plastic and p10 was a border line -- the metric never saw
# the ink at all, and it made an 8 ms drive look as good as a 200 ms one.
# Re-run `--calibrate` whenever the rig moves or the room light changes; it
# prints a replacement for these two lines. 2026-08-20 after a hub power cycle:
# (420,203)-(799,548), 379x345, aspect 1.099, swing 103.2.
CROP = (420, 203, 799, 548)

# Reference swing in this rig: solid white reads 130.4, solid black 27.2.
FULL_SWING = 103.2


def grab():
    """One exposed frame. ffmpeg's first frames are pre-exposure; keep the last."""
    d = tempfile.mkdtemp(prefix="panelshot_")
    pat = os.path.join(d, "w_%03d.png")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "dshow",
         "-video_size", SIZE, "-i", "video=" + CAM,
         "-frames:v", str(WARM), "-y", pat],
        check=True)
    last = os.path.join(d, "w_%03d.png" % WARM)
    im = Image.open(last).convert("L").copy()
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    os.rmdir(d)
    return im


def calibrate(port="COM30"):
    """Difference a solid-white panel against a solid-black one.

    The bounding box of what changed IS the active area -- no bezel, no FPC, no
    guessing. This is the only trustworthy way to set CROP: a hand-picked crop
    once scored contrast 93-100 for every drive count from 1 to 24, because p90
    was bezel plastic and p10 was the test card's border line, and the histogram
    never saw the ink at all.
    """
    import serial

    s = serial.Serial()
    s.port, s.baudrate = port, 115200
    s.timeout = 0.3
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.6)
    s.reset_input_buffer()

    def cmd(c, want, secs=50):
        s.reset_input_buffer()
        s.write((c + "\n").encode())
        s.flush()
        buf = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            b = s.read(512)
            if b:
                buf += b
            if want.encode() in buf:
                break
        return buf

    was_fast = b"FAST on" in cmd("fast", "frames=", 8)
    cmd("fast off", "OK fast", 8)
    cmd("white", "OK white", 60)
    w = np.asarray(grab(), dtype=np.float32)
    cmd("black", "OK black", 60)
    b = np.asarray(grab(), dtype=np.float32)
    cmd("white", "OK white", 60)
    if was_fast:
        cmd("fast on", "frames=", 8)
    s.close()

    d = np.abs(w - b)
    m = d > d.max() * 0.45
    rows = np.where(m.sum(axis=1) > 20)[0]
    cols = np.where(m.sum(axis=0) > 20)[0]
    if rows.size < 10 or cols.size < 10:
        print("calibration failed: nothing changed between white and black")
        return
    y0, y1 = int(rows.min()), int(rows.max())
    x0, x1 = int(cols.min()), int(cols.max())
    wl = float(w[y0:y1, x0:x1].mean())
    bl = float(b[y0:y1, x0:x1].mean())
    asp = (x1 - x0) / float(y1 - y0)
    print("active area %dx%d aspect=%.3f  white=%.1f black=%.1f swing=%.1f"
          % (x1 - x0, y1 - y0, asp, wl, bl, wl - bl))
    if not 0.8 < asp < 1.25:
        print("  aspect is off for a 200x200 panel -- check the framing")
    print("\nput these in panelshot.py:")
    print("CROP = (%d, %d, %d, %d)" % (x0, y0, x1, y1))
    print("FULL_SWING = %.1f" % (wl - bl))


def stats(g):
    """p10/p90 rather than min/max: a hot pixel or a shadow must not set the
    range. contrast = p90 - p10 is the number to compare waveforms on."""
    v = np.asarray(g, dtype=np.float32).reshape(-1)
    p10, p50, p90 = np.percentile(v, [10, 50, 90])
    return dict(mean=float(v.mean()), p10=float(p10), p50=float(p50),
                p90=float(p90), contrast=float(p90 - p10), std=float(v.std()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="glass.png")
    ap.add_argument("--calibrate", action="store_true",
                    help="drive the panel white then black and measure the "
                         "active area; prints replacement CROP/FULL_SWING")
    ap.add_argument("--inset", type=float, default=0.02,
                    help="trim this fraction off each edge before measuring, so "
                         "the bezel and the FPC never enter the histogram")
    args = ap.parse_args()

    if args.calibrate:
        calibrate()
        return

    # Throw the first grab away. A cold open reads far too dark even with 40
    # warm frames -- it produced contrast 7-9 where the very next grab of the
    # identical panel read 124-133, and one such reading was nearly taken as
    # evidence that a waveform had failed. The second grab is warm.
    grab()
    im = grab()
    x0, y0, x1, y1 = CROP

    crop = im.crop((x0, y0, x1, y1))
    w, h = crop.size
    dx, dy = int(w * args.inset), int(h * args.inset)
    inner = crop.crop((dx, dy, w - dx, h - dy))

    st = stats(inner)
    ImageOps.autocontrast(crop).resize((w * 2, h * 2)).save(args.out)
    print("saved %s  panel %dx%d aspect %.3f" % (args.out, w, h, w / float(h)))
    print("mean=%.1f p10=%.1f p50=%.1f p90=%.1f contrast=%.1f std=%.1f "
          "pct_full=%.0f"
          % (st["mean"], st["p10"], st["p50"], st["p90"], st["contrast"],
             st["std"], 100.0 * st["contrast"] / FULL_SWING))


if __name__ == "__main__":
    main()
