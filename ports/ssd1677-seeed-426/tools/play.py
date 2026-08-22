"""Stream a PNG sequence to the 4.26" panel as dithered 1-bit frames.

  python play.py COM19 C:\\clip\\frames --step 3 --drive 4

The device cannot store the clip: 488 frames of packed 800x480 mono is ~23 MB
against 8 MB of flash. So the host does the work — decode, crop, greyscale,
dither, pack, delta-encode — and the device just applies deltas and displays.

Frames go out in the panel's NATIVE landscape layout (800x480, row-major,
MSB = leftmost pixel, 1 = white), so the firmware does no conversion at all.

Each frame is sent as a changed-byte bitmap plus the new values of the changed
bytes, ~3.4x smaller than the raw frame. That is what lets the link keep up with
the panel: raw frames capped playback at 4.9 fps regardless of drive setting.
"""

import argparse
import glob
import os
import sys
import time
from collections import deque

import numpy as np
import serial
from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
STRIDE = W // 8                 # 100 bytes per row
PLANE = STRIDE * H              # 48000
CHUNK = 12288                   # must match the firmware's STREAM_CHUNK

# The device's request token. It must be a byte that cannot occur in ordinary log
# output: this was 'R' at first, and the device's own "STREAM ready" banner
# contains an R — the host matched that, started sending a frame before the
# device was reading, and the stream desynchronised before it began.
TOKEN = bytes([0x06])

# Ordered 4x4 Bayer. 17 apparent grey levels from 1 bit, and unlike the panel's
# native 4-level mode it never flashes — a differential update only drives the
# pixels that changed, so there is no global reset.
#
# Being an ORDERED matrix is also what makes delta encoding work: it is fixed to
# pixel position, so a static area dithers to identical bytes every frame and
# vanishes from the delta. Error diffusion would perturb every pixel each frame.
# Tone defaults, all chosen on glass. Ordered dithering reads washed out because
# the Bayer threshold is a flat ramp: any source sitting above mid-grey turns
# most cells white, so gamma > 1 is the corrective. Stills take a heavier hand
# than video — a single frame has no motion to carry it and wants the extra
# density.
GAMMA_VIDEO = 1.3
GAMMA_STILL = 1.55
# Native 4-level grey needs less correction than 1-bit does: the dither only
# chooses between two ADJACENT shades, so it is not fighting a flat black/white
# threshold in the first place.
GAMMA_GRAY = 1.2
CONTRAST = 1.4

BAYER4 = np.array([[0, 8, 2, 10],
                   [12, 4, 14, 6],
                   [3, 11, 1, 9],
                   [15, 7, 13, 5]], dtype=np.float32)


def load_gray(path, fit=False, rotate=0):
    """Load an image FILE as an 800x480 greyscale array."""
    return fit_image(Image.open(path), fit, rotate)


def fit_image(im, fit=False, rotate=0):
    """Any PIL image to an 800x480 greyscale array.

    Default is centre-crop, which fills the panel but discards whatever falls
    outside its 5:3. `fit` letterboxes instead, scaling the whole image inside the
    panel and padding with white — the right choice for a portrait source, where a
    crop would keep only a horizontal band. `rotate` turns the image first, for
    when the panel itself can be stood on its side.
    """
    if im.mode != "L":
        im = im.convert("L")
    if rotate:
        im = im.rotate(rotate, expand=True)
    if im.width == W and im.height == H:
        return np.asarray(im)

    if fit:
        scale = min(W / im.width, H / im.height)
        nw = max(1, int(im.width * scale + 0.5))
        nh = max(1, int(im.height * scale + 0.5))
        im = im.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("L", (W, H), 255)
        canvas.paste(im, ((W - nw) // 2, (H - nh) // 2))
        return np.asarray(canvas)

    scale = max(W / im.width, H / im.height)
    im = im.resize((max(W, int(im.width * scale + 0.5)),
                    max(H, int(im.height * scale + 0.5))), Image.LANCZOS)
    left = (im.width - W) // 2
    top = (im.height - H) // 2
    return np.asarray(im.crop((left, top, left + W, top + H)))


def apply_tone(g, gamma, contrast, highlights=255, knee=150.0):
    """Greyscale tone curve, in place on a float copy.

    `highlights` compresses ONLY the range above `knee`, mapping 255 down to the
    given value and leaving everything darker exactly as it was. Gamma and
    contrast both move the whole image, so neither can bring bright points down
    without also crushing shadow detail — this can.
    """
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


def build_threshold(matrix, w, h):
    m = (matrix + 0.5) / matrix.size * 255.0
    return np.tile(m, (h // m.shape[0] + 1, w // m.shape[1] + 1))[:h, :w]


def frame_to_bits(path, thresh, invert, gamma, contrast=1.0,
                  highlights=255, knee=150.0, fit=False, rotate=0):
    g = apply_tone(load_gray(path, fit, rotate), gamma, contrast, highlights, knee)
    white = g > thresh          # 1 = white, matching the panel's RAM convention
    if invert:
        white = ~white
    return np.packbits(white, axis=1)   # (480, 100) uint8


_label_cache = {}


def label_patch(text, scale=6, pad=6):
    """Text in black on a white box, PACKED, width padded to whole bytes.

    Packed and byte-aligned so it can be stamped straight into the frame's packed
    rows — no unpacking a 48 KB frame per label change. Cached because the string
    only changes when the displayed fps does.
    """
    hit = _label_cache.get(text)
    if hit is not None:
        return hit
    tmp = Image.new("L", (400, 24), 0)
    ImageDraw.Draw(tmp).text((1, 1), text, fill=255, font=ImageFont.load_default())
    tmp = tmp.crop(tmp.getbbox())
    tmp = tmp.resize((tmp.width * scale, tmp.height * scale), Image.NEAREST)
    ink = np.asarray(tmp) > 127
    h, w = ink.shape
    bw = ((w + 2 * pad) + 7) // 8 * 8
    patch = np.ones((h + 2 * pad, bw), dtype=bool)
    patch[pad:pad + h, pad:pad + w] = ~ink
    packed = np.packbits(patch, axis=1)
    _label_cache[text] = packed
    return packed


def stamp(frame, packed, y0=14, bx0=1):
    """Overwrite packed label bytes into a (480,100) packed frame, in place."""
    h, bw = packed.shape
    frame[y0:y0 + h, bx0:bx0 + bw] = packed


def park_rim(plane):
    """Force the outer 1 px to white (1) so live video never inks the rim.

    Same rule as StarPilot DISP_BORDER_W: edge ink on this glass walks into
    gate/source streaks. Park once as white and leave those bits unchanged.
    `plane` is packed (H, STRIDE), MSB = leftmost.
    """
    plane[0, :] = 0xFF
    plane[-1, :] = 0xFF
    plane[:, 0] |= 0x80
    plane[:, -1] |= 0x01
    return plane


def encode_delta(cur, prev):
    """(changed-byte bitmap, new values of changed bytes).

    The count of changed bytes is never sent — the device recovers it by counting
    set bits, so there is no length field for the two ends to disagree about.
    """
    d = cur != prev
    return np.packbits(d).tobytes(), cur[d].tobytes()


def to_gray_planes(path, invert, gamma, contrast, levels_thresh,
                   highlights=255, knee=150.0, fit=False, rotate=0):
    """Quantise to the panel's four NATIVE grey levels, dithering between them.

    Returns (low_plane, high_plane) packed bytes. Code 0 is black and 3 is white,
    verified on glass; 0x24 carries the LOW bit and 0x26 the HIGH bit, which is
    the opposite of the obvious assumption.

    Bayer is applied to the FRACTION between adjacent levels rather than as a
    black/white threshold, so it only ever picks between two neighbouring shades.
    That is what makes it far smoother than 1-bit dithering: the error it has to
    diffuse is a third as large.
    """
    g = apply_tone(load_gray(path, fit, rotate), gamma, contrast, highlights, knee)
    if invert:
        g = 255.0 - g

    v = g / 255.0 * 3.0
    base = np.floor(v)
    code = base + (v - base > levels_thresh)
    code = np.clip(code, 0, 3).astype(np.uint8)
    return (np.packbits(code & 1, axis=1).ravel(),
            np.packbits(code >> 1, axis=1).ravel())


def panel_fps(drive):
    """Measured timing law: waveform + fixed SPI + fixed per-frame overhead.

    Held to within 0.6% across drives 2..10, including drive 9 which was
    predicted before it was ever run. See docs/PLAYER-ROADMAP.md.
    """
    # 36.0 = SPI (20 ms for both planes at 40 MHz) plus ~16 ms of per-frame
    # overhead. FITTED to measurement, not assumed: frame time minus waveform
    # came out 35.8/35.8/35.9/36.1/36.1/36.9 ms across drives 12/9/8/7/5/2.
    #
    # The overhead term was 10.8 ms when SPI ran at 20 MHz. Carrying that over
    # made this optimistic by ~4%, enough for pick_drive to choose a mode that
    # could not actually hold real-time.
    return 1000.0 / ((drive + 1) * 9.92 + 36.0)


def pick_drive(source_fps, step, tol=0.02):
    """Slowest drive (so: darkest, highest contrast) that still plays real-time.

    Deliberately not the fastest available — slower modes look better, so
    overshooting costs picture quality, while undershooting makes playback drag.
    25 fps at step 3 lands on mode 6, 30 fps at step 4 on mode 7.
    """
    need = source_fps / step
    for d in range(12, 1, -1):
        if panel_fps(d) >= need * (1.0 - tol):
            return d
    return 2


def wait_token(s):
    """Read until the device's request token, skipping any banner text."""
    tok = s.read(1)
    while tok and tok != TOKEN:
        tok = s.read(1)
    return bool(tok)


def send_blob(s, blob):
    for off in range(0, len(blob), CHUNK):
        if not wait_token(s):
            return False
        try:
            s.write(blob[off:off + CHUNK])
        except serial.SerialTimeoutException:
            # The board stopped consuming — almost certainly a reset mid-stream.
            # Without write_timeout set this blocks FOREVER on a full OS buffer,
            # which is what turned a device reboot into a hung command.
            print("\nwrite timed out - device stopped reading")
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("folder")
    ap.add_argument("--step", type=int, default=3,
                    help="use every Nth source frame (source is 25 fps)")
    ap.add_argument("--drive", type=int, default=None,
                    help="waveform drive frames; default picked from --source-fps")
    ap.add_argument("--source-fps", type=float, default=25.0,
                    help="frame rate of the stills, used to choose the drive mode")
    # Left as None so a still can take a different default from video without
    # overriding a value the user actually asked for.
    ap.add_argument("--gamma", type=float, default=None,
                    help=f">1 DARKENS midtones; default {GAMMA_VIDEO} video, "
                         f"{GAMMA_STILL} stills")
    ap.add_argument("--contrast", type=float, default=None,
                    help=f"linear stretch about mid-grey; default {CONTRAST}")
    # +/-17V is the datasheet maximum for VSH1/VSL and gives a visibly darker
    # black than the 15V default, which matters more the faster the mode. It is
    # also the ceiling: above it frame rates are unchanged and the contrast gain
    # is marginal, so there is no reason to leave spec. Stock is "41 32".
    ap.add_argument("--volts", default="4B 3A",
                    help='VSH1 VSL in hex; default "4B 3A" = +/-17V, stock "41 32"')
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--repeat", type=int, default=1, help="play the clip N times")
    ap.add_argument("--label", default=None,
                    help='corner text prefix; defaults to the drive number, "" for none')
    ap.add_argument("--highlights", type=float, default=255,
                    help="map white down to this value, leaving shadows untouched")
    ap.add_argument("--knee", type=float, default=150.0,
                    help="level above which --highlights starts compressing")
    ap.add_argument("--fit", action="store_true",
                    help="letterbox the whole image instead of centre-cropping it")
    ap.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270],
                    help="rotate before fitting, for a panel stood on its side")
    ap.add_argument("--gray", action="store_true",
                    help="single image in the panel's native 4 grey levels")
    ap.add_argument("--greset", type=int, default=20,
                    help="--gray: frames driving every code to a common black")
    ap.add_argument("--gstep", type=int, default=10,
                    help="--gray: frames per step back toward white")
    ap.add_argument("--no-pace", action="store_true",
                    help="send as fast as the panel accepts, for benchmarking")
    ap.add_argument("--sink", action="store_true",
                    help="measure the USB link alone, without driving the panel")
    args = ap.parse_args()

    # A single image file is allowed as well as a folder. Stills are not paced by
    # anything, so they get a slow, high-contrast mode rather than a chosen one.
    still = os.path.isfile(args.folder)

    # Native 4-level grey. Only for stills: it is a full repaint with a common
    # reset, so it flashes once per update.
    if args.gray:
        if not still:
            sys.exit("--gray is for a single image, not a sequence")
        gamma = GAMMA_GRAY if args.gamma is None else args.gamma
        contrast = CONTRAST if args.contrast is None else args.contrast
        # Bayer, scaled to the 0..1 fraction between two adjacent levels.
        thr = (BAYER4 + 0.5) / BAYER4.size
        thr = np.tile(thr, (H // 4 + 1, W // 4 + 1))[:H, :W]
        low, high = to_gray_planes(args.folder, args.invert, gamma, contrast, thr,
                                   args.highlights, args.knee, args.fit,
                                   args.rotate)

        s = serial.Serial()
        s.port, s.baudrate, s.timeout = args.port, 115200, 5
        s.dtr = s.rts = False
        s.open()
        time.sleep(0.3)
        s.write(f"volts {args.volts}\n".encode())
        time.sleep(0.4)
        s.reset_input_buffer()
        s.write(f"gstill {args.greset} {args.gstep}\n".encode())
        # Low plane first, then high — the order the firmware reads them in.
        ok = send_blob(s, low.tobytes()) and send_blob(s, high.tobytes())
        print("sent" if ok else "device stopped asking for data")
        time.sleep(3.0)
        print(s.read_all().decode("utf-8", "replace").strip())
        s.close()
        return

    if args.gamma is None:
        args.gamma = GAMMA_STILL if still else GAMMA_VIDEO
    if args.contrast is None:
        args.contrast = CONTRAST
    if still:
        files = [args.folder]
        if args.drive is None:
            args.drive = 12
        if args.label is None:
            args.label = ""      # no fps counter burnt into a still
    else:
        files = sorted(glob.glob(os.path.join(args.folder, "*.png")))
        if not files:
            sys.exit("no PNGs in " + args.folder)
        files = files[::args.step]
        if args.drive is None:
            args.drive = pick_drive(args.source_fps, args.step)
            print(f"drive {args.drive} chosen for {args.source_fps:g} fps "
                  f"at step {args.step} "
                  f"(needs {args.source_fps/args.step:.2f}, "
                  f"panel does {panel_fps(args.drive):.2f})")

    thresh = build_threshold(BAYER4, W, H)

    # Dither up front. Decoding a 291 MB PNG set mid-playback would starve the
    # panel and make the frame rate a measure of Pillow rather than of the
    # display. Delta encoding, by contrast, happens per frame during playback —
    # it has to, because the on-screen fps readout changes what each frame
    # contains.
    print("dithering...", end="", flush=True)
    t0 = time.time()
    bits = [frame_to_bits(f, thresh, args.invert, args.gamma, args.contrast,
                          args.highlights, args.knee, args.fit, args.rotate)
            for f in files]
    print(f" {len(bits)} frames in {time.time()-t0:.1f}s")

    prefix = str(args.drive) if args.label is None else args.label

    # Deassert DTR/RTS before opening or the board resets on connect.
    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 5
    s.write_timeout = 5      # never block forever if the board resets
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()

    if args.volts:
        s.write(f"volts {args.volts}\n".encode())
        time.sleep(0.4)
    s.write(f"drive {args.drive}\n".encode())
    time.sleep(0.5)
    # Deltas are relative, so the glass must actually start at the white the
    # device assumes. Without this the first frame is rebuilt on top of whatever
    # was on screen and the error never washes out.
    if not args.sink:
        s.write(b"white\n")
        time.sleep(2.5)
    s.reset_input_buffer()
    s.write(b"sink\n" if args.sink else b"stream\n")
    # Deliberately no drain after this point: the device may already be sending
    # tokens and discarding one would deadlock. TOKEN cannot occur in the banner,
    # so simply scanning past the text is safe.

    # Real-time pacing. Without it the host ships frames as fast as the panel
    # accepts them, so a mode FASTER than the source needs plays sped up rather
    # than correct — drive 2 at 14.9 fps against a 12.5 fps requirement runs 19%
    # quick. A mode slower than the requirement just falls behind, which a clock
    # cannot fix and this does not try to.
    frame_dt = 0.0 if args.no_pace else args.step / args.source_fps
    if frame_dt:
        print(f"pacing to {1/frame_dt:.2f} fps")

    sent = 0
    enc_bytes = 0
    prev = np.full((H, STRIDE), 0xFF, dtype=np.uint8)   # device starts white
    recent = deque(maxlen=8)
    t0 = time.time()
    due = t0
    try:
        for _ in range(args.repeat):
            for fr in bits:
                cur = fr.copy()
                if prefix != "":
                    fps = ((len(recent) - 1) / (recent[-1] - recent[0])
                           if len(recent) > 1 else 0.0)
                    stamp(cur, label_patch(f"{prefix}   {fps:4.1f} fps"
                                           if fps else f"{prefix}    -.- fps"))
                bmp, lit = encode_delta(cur, prev)
                enc_bytes += len(bmp) + len(lit)
                if frame_dt:
                    now = time.time()
                    if now < due:
                        time.sleep(due - now)
                    # Never let `due` fall behind the wall clock: if the panel
                    # cannot keep up, the deficit must not accumulate into a
                    # burst of catch-up frames once it does.
                    due = max(due + frame_dt, time.time())
                if not send_blob(s, bmp) or not send_blob(s, lit):
                    print("\ndevice stopped asking for data")
                    return
                prev = cur
                sent += 1
                recent.append(time.time())
                if sent % 10 == 0:
                    el = time.time() - t0
                    print(f"\r{sent} frames  {sent/el:5.2f} fps", end="", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        # Take the elapsed time BEFORE the shutdown wait below, or that wait is
        # counted as playback and every rate comes out ~20% low.
        el = time.time() - t0
        # A short read is how the firmware knows to leave stream mode.
        time.sleep(4.5)
        print(f"\ndone: {sent} frames in {el:.1f}s = {sent/el:.2f} fps"
              f"  ({enc_bytes/max(sent,1):.0f} B/frame, "
              f"{PLANE*sent/max(enc_bytes,1):.2f}x smaller)")
        print(s.read_all().decode("utf-8", "replace").strip())
        s.close()


if __name__ == "__main__":
    main()
