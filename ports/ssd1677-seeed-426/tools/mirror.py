"""Mirror a live screen region onto the 4.26" panel.

  python mirror.py COM19 --select              # drag a region, then mirror it
  python mirror.py COM19 --full                # whole primary monitor
  python mirror.py COM19 --region 100,80,1280,720
  python mirror.py COM19 --select --rotate 270 # panel stood on its side

Ctrl-C to stop. Everything about the wire format is shared with play.py — the
same dither, delta encoding and handshake — so the only new part is where the
frames come from.

Capture runs on its own thread. Grabbing is ~8 ms and the panel needs 80-130 ms,
so a serial loop would idle the display for the whole grab; instead the newest
captured frame is always waiting when the device asks for one. That also means
frames are DROPPED rather than queued when the source outruns the panel, which is
what you want for a live mirror — it stays current instead of falling behind.
"""

import argparse
import sys
import threading
import time

import numpy as np
import serial
from PIL import Image

import play
from play import (CHUNK, PLANE, H, STRIDE, W, apply_tone, build_threshold,
                  encode_delta, fit_image, park_rim, send_frame)

try:
    import mss
except ImportError:
    sys.exit("pip install mss")


def select_region():
    """Drag a rectangle over a dimmed fullscreen overlay. Returns (l, t, w, h)."""
    import tkinter as tk
    box = {}
    root = tk.Tk()
    root.attributes("-fullscreen", True)
    root.attributes("-alpha", 0.25)
    root.configure(bg="black")
    root.title("drag a region")
    cv = tk.Canvas(root, cursor="cross", bg="black", highlightthickness=0)
    cv.pack(fill="both", expand=True)
    start = {}

    def down(e):
        start["x"], start["y"] = e.x_root, e.y_root
        cv.delete("sel")

    def move(e):
        cv.delete("sel")
        cv.create_rectangle(start["x"], start["y"], e.x_root, e.y_root,
                            outline="white", width=2, tag="sel")

    def up(e):
        x0, x1 = sorted((start["x"], e.x_root))
        y0, y1 = sorted((start["y"], e.y_root))
        if x1 - x0 > 20 and y1 - y0 > 20:
            box.update(left=x0, top=y0, width=x1 - x0, height=y1 - y0)
        root.destroy()

    cv.bind("<ButtonPress-1>", down)
    cv.bind("<B1-Motion>", move)
    cv.bind("<ButtonRelease-1>", up)
    root.bind("<Escape>", lambda e: root.destroy())
    root.mainloop()
    if not box:
        sys.exit("no region selected")
    return box


class Capturer(threading.Thread):
    """Keeps `self.latest` at the most recent grab. Old frames are discarded."""

    def __init__(self, mon, fit, rotate):
        super().__init__(daemon=True)
        self.mon, self.fit, self.rotate = mon, fit, rotate
        self.latest = None
        self.running = True
        self.grabs = 0

    def run(self):
        with mss.mss() as sct:
            while self.running:
                raw = sct.grab(self.mon)
                im = Image.frombytes("RGB", raw.size, raw.bgra, "raw", "BGRX")
                self.latest = fit_image(im, self.fit, self.rotate)
                self.grabs += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--select", action="store_true", help="drag to choose a region")
    g.add_argument("--full", action="store_true", help="whole primary monitor")
    g.add_argument("--region", help="left,top,width,height")
    ap.add_argument("--drive", type=int, default=7)
    ap.add_argument("--volts", default="4B 3A")
    ap.add_argument("--gamma", type=float, default=play.GAMMA_VIDEO)
    ap.add_argument("--contrast", type=float, default=play.CONTRAST)
    ap.add_argument("--highlights", type=float, default=255)
    ap.add_argument("--knee", type=float, default=150.0)
    ap.add_argument("--fit", action="store_true",
                    help="letterbox the region instead of centre-cropping it")
    ap.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270])
    args = ap.parse_args()

    if args.select:
        mon = select_region()
    elif args.region:
        l, t, w, h = (int(v) for v in args.region.split(","))
        mon = {"left": l, "top": t, "width": w, "height": h}
    else:
        with mss.mss() as sct:
            mon = dict(sct.monitors[1])
    print(f"region {mon['width']}x{mon['height']} at "
          f"({mon['left']},{mon['top']})  drive {args.drive}")

    thresh = build_threshold(play.BAYER4, W, H)

    cap = Capturer(mon, args.fit, args.rotate)
    cap.start()
    while cap.latest is None:
        time.sleep(0.02)

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 115200, 5
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.3)
    s.write(f"volts {args.volts}\n".encode())
    time.sleep(0.4)
    s.write(f"drive {args.drive}\n".encode())
    time.sleep(0.4)
    # Deltas are relative, so the glass has to start at the white the device
    # assumes.
    s.write(b"white\n")
    time.sleep(2.5)
    s.reset_input_buffer()
    s.write(b"stream\n")

    prev = np.full((H, STRIDE), 0xFF, dtype=np.uint8)
    park_rim(prev)
    sent = 0
    t0 = time.time()
    try:
        while True:
            g = apply_tone(cap.latest, args.gamma, args.contrast,
                           args.highlights, args.knee)
            cur = np.packbits(g > thresh, axis=1)
            park_rim(cur)
            bmp, lit = encode_delta(cur, prev)
            if not send_frame(s, bmp, lit):
                print("\ndevice stopped asking for data")
                break
            prev = cur
            sent += 1
            if sent % 5 == 0:
                el = time.time() - t0
                print(f"\r{sent} frames  {sent/el:5.2f} fps   "
                      f"(captured {cap.grabs}, dropped {cap.grabs-sent})",
                      end="", flush=True)
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        cap.running = False
        el = time.time() - t0
        print(f"\n{sent} frames in {el:.1f}s = {sent/max(el,0.001):.2f} fps")
        time.sleep(4.5)
        print(s.read_all().decode("utf-8", "replace").strip())
        s.close()


if __name__ == "__main__":
    main()
