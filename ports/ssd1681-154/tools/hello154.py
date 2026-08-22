#!/usr/bin/env python3
"""Push a rendered 200x200 test card to the xiao-154 panel over the wire.

  python tools\\hello154.py COM30                  # stream path (mode 2, seq)
  python tools\\hello154.py COM30 --full           # `frame`: one full 0xF7 each
  python tools\\hello154.py COM30 --text "HELLO"   # your own words
  python tools\\hello154.py COM30 --frames 12

Proves the host->wire->panel path end to end without flashing anything. Same
framing as webcam.py: one ACK, then flags + n_lit + RLE occupancy + lit bytes.

Bit convention matches the firmware: 1 = white, 0 = black, MSB = leftmost
pixel, row-major at 25 bytes per row.

Two paths, and the difference is the whole diagnostic:

  --full  the device applies each frame with SEQ_FULL 0xF7, the mode-1 refresh
          already measured good on this glass. If this lands, the wire, the
          encoder and the framing are all fine.
  stream  the device applies each frame with its live `seq` (0xFC self-powered
          mode 2 by default, 0x0C rails-up fast). If --full lands and this does
          not, the fault is the mode-2 sequence, not the wire.

The device also renders its own card with no host involved at all: `card` over
serial, and `blink N` to flip it with the mode-2 sequence. Start there if
neither path below shows anything.
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np
import serial
from PIL import Image, ImageDraw, ImageFont

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import webcam as wv  # noqa: E402  (framing + handshake helpers)

W = H = 200
WB = W // 8            # 25
PLANE = WB * H         # 5000


def park_rim(rows):
    """Match firmware park_rim(): outer rim stays white."""
    rows[0, :] = 0xFF
    rows[H - 1, :] = 0xFF
    rows[:, 0] |= 0x80
    rows[:, WB - 1] |= 0x01
    return rows


def pick_font(size):
    for name in ("arialbd.ttf", "ariblk.ttf", "arial.ttf", "consolab.ttf",
                 "cour.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except Exception:
            continue
    return ImageFont.load_default()


def render(text, tag=""):
    """Unmistakable test card: border, split text, checker corners, diagonals."""
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)

    d.rectangle([0, 0, W - 1, H - 1], outline=0, width=3)
    d.rectangle([8, 8, W - 9, H - 9], outline=0, width=1)

    # 8 px checker in two corners -- shows byte alignment and any x shear
    for cx, cy in ((12, 12), (W - 44, H - 44)):
        for a in range(4):
            for b in range(4):
                if (a + b) % 2 == 0:
                    d.rectangle([cx + a * 8, cy + b * 8,
                                 cx + a * 8 + 7, cy + b * 8 + 7], fill=0)

    words = text.split() or ["HELLO"]
    if len(words) == 1:
        words.append("")
    size = 30
    f = pick_font(size)
    while size > 10:
        f = pick_font(size)
        if max(d.textlength(w, font=f) for w in words) <= W - 28:
            break
        size -= 2

    y = 62
    for w in words:
        tw = d.textlength(w, font=f)
        d.text(((W - tw) / 2, y), w, fill=0, font=f)
        y += size + 6

    # A per-frame tag proves each frame really landed rather than one sticking.
    if tag:
        sf = pick_font(14)
        d.text((14, H - 66), tag, fill=0, font=sf)

    # diagonals: any address wrap or row-stride error breaks the straight line
    d.line([14, H - 52, W - 15, H - 16], fill=0, width=2)
    d.line([14, H - 16, W - 15, H - 52], fill=0, width=2)

    a = np.asarray(im)
    bits = (a > 127)                       # True = white = 1
    packed = np.packbits(bits, axis=-1)    # MSB first -> (200, 25)
    return packed.astype(np.uint8)


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate = port, 115200
    s.timeout, s.write_timeout = 4, 8
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.4)
    s.reset_input_buffer()
    return s


def check_panel(s):
    """Refuse to pack 5000-byte planes at whatever else may be flashed."""
    s.write(b"status\n")
    s.flush()
    ok, raw = wv.wait_bytes(s, b"STATUS", 3.0, to_nl=True)
    line = raw.decode("utf-8", "replace").strip()
    print("  %s" % (line[-170:] or "(no STATUS)"))
    if not ok:
        return False
    rep = wv.panel_from_text(raw)
    if rep and rep != "xiao-154":
        print("device reports panel=%s. This tool only packs xiao-154." % rep)
        return False
    if not rep:
        print("  (no panel= token — old firmware; continuing)")
    return True


def send_full(s, args, target, seed):
    """`frame` command: one plane, applied with the proven 0xF7 full refresh."""
    base = seed
    sent = 0
    for i in range(args.frames):
        cur = render(args.text, "frame %d" % i).reshape(-1).copy()
        s.timeout = 4
        s.write(b"frame\n")
        s.flush()
        ok, raw = wv.wait_bytes(s, b"FRAME ready", 6.0, to_nl=True)
        if not ok:
            print("no FRAME ready: %s" % raw.decode("utf-8", "replace")[-200:])
            break
        s.timeout = 0.2
        if not wv.wait_token(s, timeout=6.0):
            print("no ACK for frame %d" % i)
            break
        bmp, lit = wv.encode_delta(cur, base)
        blob = wv.pack_wire2(bmp, lit)
        try:
            s.write(blob)
            s.flush()
        except serial.SerialTimeoutException:
            print("write timeout at frame %d" % i)
            break
        s.timeout = 8
        ok, raw = wv.wait_bytes(s, b"FRAME done", 20.0, to_nl=True)
        print("  frame %d: occ=%d lit=%d blob=%d  %s"
              % (i, len(bmp), len(lit), len(blob),
                 raw.decode("utf-8", "replace").strip()[-70:]))
        if not ok:
            break
        base = cur
        sent += 1
    return sent


def send_stream(s, args, target, seed):
    """`stream` path: the device applies each frame with its live seq."""
    s.write(b"stream\n")
    s.flush()
    wire, ack_pending, banner = wv.stream_handshake(s, 12.0)
    print("  %s" % banner.strip()[-170:])
    if wire is None:
        return None
    print("wire=%d  ack_already_held=%s" % (wire, ack_pending))

    s.timeout = 0.05
    prev = [seed.copy(), seed.copy()]
    sent = 0
    for i in range(args.frames):
        cur = render(args.text, "frame %d" % i).reshape(-1).copy()
        base = prev[(i + 1) % 2]
        bmp, lit = wv.encode_delta(cur, base)
        blob = wv.pack_wire2(bmp, lit)
        if ack_pending:
            ack_pending = False        # handshake already took frame 0's ACK
        elif not wv.wait_token(s, timeout=6.0):
            print("no ACK for frame %d" % i)
            break
        try:
            s.write(blob)
            s.flush()
        except serial.SerialTimeoutException:
            print("write timeout at frame %d" % i)
            break
        prev[i % 2] = cur
        sent += 1
        print("  frame %d: occ=%d lit=%d blob=%d"
              % (i, len(bmp), len(lit), len(blob)))

    s.timeout = 4
    s.write(bytes([0x18]))   # CAN -> leave stream
    s.flush()
    _ok, raw = wv.wait_bytes(s, b"STREAM done", 8.0, to_nl=True)
    print("  %s" % raw.decode("utf-8", "replace").strip()[-170:])
    return sent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM30")
    ap.add_argument("--text", default="HELLO WORLD")
    ap.add_argument("--frames", type=int, default=6,
                    help="stream: enough to cycle trail/chase. full: each is "
                         "a ~2 s refresh, so keep it small")
    ap.add_argument("--full", action="store_true",
                    help="use the `frame` command: SEQ_FULL 0xF7 per frame, the "
                         "only sequence proven on this glass")
    ap.add_argument("--no-white", action="store_true",
                    help="skip the deghost; the firmware whites on its own if "
                         "it does not already know the glass is white")
    args = ap.parse_args()
    if args.full and args.frames > 6:
        args.frames = 6

    target = render(args.text).reshape(-1).copy()
    assert target.size == PLANE, target.size
    black_px = int((PLANE * 8) - np.unpackbits(target).sum())
    print("rendered %dx%d  plane=%d bytes  black=%d px (%.1f%%)"
          % (W, H, PLANE, black_px, 100.0 * black_px / (W * H)))

    # The device seeds both stream buffers to 0xFF then park_rim(). Match
    # exactly, or the first delta lands on the wrong base.
    seed = park_rim(np.full((H, WB), 0xFF, np.uint8)).reshape(-1).copy()

    s = open_port(args.port)
    try:
        if not check_panel(s):
            sys.exit(1)

        if not args.no_white:
            print("white (full 0xF7, ~2 s)...")
            s.write(b"white\n")
            s.flush()
            ok, raw = wv.wait_bytes(s, b"OK white", 30.0, to_nl=True)
            print("  %s" % (raw.decode("utf-8", "replace").strip()[-120:]
                            or "(no reply)"))
            if not ok:
                sys.exit("no OK white -- is xiao-154 flashed on %s?" % args.port)

        if args.full:
            sent = send_full(s, args, target, seed)
        else:
            sent = send_stream(s, args, target, seed)
        if sent is None:
            sys.exit("no STREAM ready")
        print("sent %d/%d frames via %s"
              % (sent, args.frames, "frame/0xF7" if args.full else "stream/seq"))
    finally:
        s.close()


if __name__ == "__main__":
    main()
