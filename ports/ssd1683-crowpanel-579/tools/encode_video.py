"""Encode a PNG sequence into CrowPanel 5.79 frames, delta-compressed.

    python encode_video.py C:\\clip\\frames out.bin
    python encode_video.py C:\\clip\\frames --analyze      # sizes only, no file

Produces the panel's OWN buffer layout, so the device does no transformation at
all — it applies the delta and blits.

Geometry: the source is scaled to the full 792 px visible width and the BOTTOM is
cropped away, which is what fills a 2.9:1 panel from 16:9 material without
squashing anyone. 853x480 -> 792x446, keep the top 272 rows.

Two things about this panel that the encoder has to know, and that make a naive
frame come out wrong:

1. The framebuffer is COLUMN-MAJOR. The controller wants every row of byte-column
   0, then every row of column 1. Index is col * 272 + row, not row * 100 + col.

2. Logical width is 800, but only 792 pixels exist. The two SSD1683s are cascaded
   and there is an 8-pixel DEAD GAP at x = 396..403 where they join. Source pixels
   396+ must be pushed 8 to the right or the whole right half lands 8 px off.

Wire/file format, per frame:
    3400 bytes   bitmap, one bit per framebuffer byte, set = that byte changed
    N bytes      the new value of each changed byte, in order

N is never transmitted: both ends derive it by counting set bits, so there is no
length field to fall out of step. This only works because the dither is a fixed
ordered matrix — a static area produces byte-identical output every frame and drops
out of the delta entirely. Error diffusion would perturb every pixel and leave
nothing to remove.
"""

import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image

PANEL_W = 800          # logical, includes the dead gap
VISIBLE_W = 792
PANEL_H = 272
FB_WBYTES = PANEL_W // 8          # 100 byte columns
PLANE_BYTES = FB_WBYTES * PANEL_H  # 27200
BITMAP_BYTES = PLANE_BYTES // 8    # 3400
SEAM_X = 396
SEAM_PX = 8

# 4x4 Bayer, the same matrix the firmware uses for its ghosts and circle field.
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

# CLUSTERED DOT, 4x4. Neighbouring thresholds are adjacent in VALUE as well as in
# space, so ink grows as compact blobs instead of the scattered single pixels Bayer
# produces.
#
# This matters on e-paper specifically: an isolated black pixel surrounded by white
# is the hardest case there is, because lateral field spread from its neighbours
# fights it, and at a short waveform it never fully resolves. A regular lattice of
# such pixels that resolves unevenly reads as structured banding — which is why the
# game's solid shapes look clean at the same drive count that makes video band.
CLUSTER = [[12, 5, 6, 13], [4, 0, 1, 7], [11, 3, 2, 8], [15, 10, 9, 14]]

# 2x2 Bayer: coarsest option, only 5 effective grey levels but the largest clusters.
BAYER2 = [[0, 2], [3, 1]]

def bayer(n):
    """Recursive dispersed-dot Bayer of size n (a power of two).

        M1 = [[0]],  M2n = [[4M, 4M+2], [4M+3, 4M+1]]

    Bigger n means more grey levels (n*n) and finer texture: 8x8 gives 64 levels
    against 4x4's 16, so gradients stop showing the matrix. Built rather than
    hardcoded — an 8x8 typed by hand is 64 chances to transpose a digit.
    """
    m = [[0]]
    size = 1
    while size < n:
        m = [[4 * v for v in row] + [4 * v + 2 for v in row] for row in m] +             [[4 * v + 3 for v in row] + [4 * v + 1 for v in row] for row in m]
        size *= 2
    return m


# Classical 8x8 CLUSTERED-DOT screen, oriented on the diagonal.
#
# Two dot centres per cell arranged diagonally, which is a ~45 degree screen — the
# same trick print halftones use, because an axis-aligned grid is far more visible to
# the eye than a diagonal one. 64 levels against cluster4's 16, so gradients stop
# posterising, while the clustering is what keeps it resolvable on e-paper.
#
# This is the middle ground between the two failures measured on glass: dispersed
# matrices (bayer4/bayer8) smear because isolated single pixels never resolve against
# lateral field spread, and cluster4 resolves but reads as blocky 4x4 texture.
CLUSTER8 = [
    [24, 10, 12, 26, 35, 47, 49, 37],
    [ 8,  0,  2, 14, 45, 59, 61, 51],
    [22,  6,  4, 16, 43, 57, 63, 53],
    [30, 20, 18, 28, 33, 41, 55, 39],
    [34, 46, 48, 36, 25, 11, 13, 27],
    [44, 58, 60, 50,  9,  1,  3, 15],
    [42, 56, 62, 52, 23,  7,  5, 17],
    [32, 40, 54, 38, 31, 21, 19, 29],
]
# A transposed digit here would produce subtly wrong tone rather than an error, so
# check it is a genuine permutation of 0..63 at import.
assert sorted(v for row in CLUSTER8 for v in row) == list(range(64)), "CLUSTER8 broken"

DITHERS = {
    "bayer2": BAYER2,        # coarsest, 5 levels
    "bayer4": BAYER,         # 16 levels, scatters isolated pixels
    "bayer8": bayer(8),      # 64 levels, finest gradients
    "cluster4": CLUSTER,     # compact blobs, resolves best but reads as texture
    "cluster8": CLUSTER8,    # 45-degree screen, 64 levels — the balanced choice
}

# Judged on glass for video on the sibling panel. Ordered dithering reads washed
# out because the Bayer threshold is a flat ramp, so anything above mid-grey turns
# most cells white; gamma > 1 is the correction.
GAMMA = 1.3
CONTRAST = 1.4


def build_lut(gamma, contrast):
    i = np.arange(256, dtype=np.float64) / 255.0
    v = np.power(i, gamma)
    v = (v - 0.5) * contrast + 0.5
    return np.clip(v * 255.0 + 0.5, 0, 255).astype(np.uint8)


def build_threshold(h=PANEL_H, w=VISIBLE_W, name="bayer4"):
    """The 4x4 Bayer matrix tiled to a grid and scaled to 0..255.

    Tiled over the FINAL pixel grid, so the pattern is positionally fixed on the
    panel. That is what makes a static area dither to byte-identical output every
    frame and drop out of the delta completely.
    """
    m = np.array(DITHERS[name], dtype=np.uint16)
    n = m.shape[0]
    step = 256 // (n * n)
    b = m * step + step // 2
    return np.tile(b, (h // n + 1, w // n + 1))[:h, :w]


# Portrait canvas, matching the firmware's Paint rotation-90 mapping.
PORTRAIT_W = PANEL_H            # 272 across
PORTRAIT_H = VISIBLE_W          # 792 along


def portrait_x_map():
    """Native X for each portrait y, replicating Paint_SetPixel's rotate=90 case.

        if (Ypoint >= 396) Ypoint += 8;
        X = 800 - Ypoint - 1;  Y = Xpoint;

    The +8 opens the dead gap at the cascade seam; the reversal is the panel's own
    scan direction. Getting either wrong mirrors the image or shifts half of it.
    """
    py = np.arange(PORTRAIT_H)
    return (PANEL_W - 1) - (py + np.where(py >= SEAM_X, SEAM_PX, 0))


def image_to_fb_rot(im, lut, thresh, xmap, rot):
    """Rotated: fit the source to 272 px ACROSS the panel, centred along its length.

    A 16:9 source turned 90 degrees is 9:16, so 272 wide gives ~483 tall out of the
    792 available. The bands above and below stay white and never enter the delta,
    which is where the bandwidth saving comes from — the active area is ~61% of the
    panel instead of 100%.
    """
    if im.mode != "L":
        im = im.convert("L")
    # The canvas reaches the glass turned 90 clockwise (xmap), so rot=90 cancels
    # that and rot=0 leaves the picture turned 90 clockwise on the glass.
    turn = {90: Image.ROTATE_90, 180: Image.ROTATE_180, 270: Image.ROTATE_270}
    if rot in turn:
        im = im.transpose(turn[rot])
    sw, sh = im.size

    nh = max(1, round(sh * PORTRAIT_W / sw))
    im = im.resize((PORTRAIT_W, nh), Image.LANCZOS)
    if nh > PORTRAIT_H:
        top = (nh - PORTRAIT_H) // 2
        im = im.crop((0, top, PORTRAIT_W, top + PORTRAIT_H))
        nh = PORTRAIT_H

    canvas = Image.new("L", (PORTRAIT_W, PORTRAIT_H), 255)   # white letterbox
    canvas.paste(im, (0, (PORTRAIT_H - nh) // 2))

    v = lut[np.asarray(canvas, dtype=np.uint8)]              # (792, 272)
    black = v < thresh                                       # portrait grid

    # Scatter the portrait grid into native orientation: nat[Y=px, X=xmap[py]].
    nat = np.zeros((PANEL_H, PANEL_W), dtype=bool)
    nat[:, xmap] = black.T

    packed = np.packbits(~nat, axis=1)                       # (272, 100), 1 = white
    return np.ascontiguousarray(packed.T).reshape(-1)


def frame_to_fb_rot(path, lut, thresh, xmap, rot):
    return image_to_fb_rot(Image.open(path), lut, thresh, xmap, rot)


def image_to_fb(im, lut, thresh):
    """One image -> 27200 bytes in the panel's own layout, as a numpy uint8 array.

    Vectorised because the naive per-pixel loop is 215k iterations a frame, which
    put a 488-frame encode into the tens of minutes.
    """
    if im.mode != "L":
        im = im.convert("L")
    sw, sh = im.size

    # Scale to the full visible width, then keep the TOP crop.
    nh = max(1, round(sh * VISIBLE_W / sw))
    im = im.resize((VISIBLE_W, nh), Image.LANCZOS)
    if nh >= PANEL_H:
        im = im.crop((0, 0, VISIBLE_W, PANEL_H))
    else:  # source shorter than the panel: pad below rather than stretch
        pad = Image.new("L", (VISIBLE_W, PANEL_H), 255)
        pad.paste(im, (0, 0))
        im = pad

    v = lut[np.asarray(im, dtype=np.uint8)]        # (272, 792) tone-mapped
    black = v < thresh                             # ordered-dither threshold

    # Widen 792 -> 800 by opening the 8-column dead gap at the cascade seam.
    logical = np.zeros((PANEL_H, PANEL_W), dtype=bool)
    logical[:, :SEAM_X] = black[:, :SEAM_X]
    logical[:, SEAM_X + SEAM_PX :] = black[:, SEAM_X:]

    # 1 = white. packbits is big-endian per byte, which matches the panel's
    # 0x80 >> (x & 7) bit order exactly.
    packed = np.packbits(~logical, axis=1)         # (272, 100)
    # Column-major: index = col * 272 + row, i.e. the transpose flattened.
    return np.ascontiguousarray(packed.T).reshape(-1)


def frame_to_fb(path, lut, thresh):
    return image_to_fb(Image.open(path), lut, thresh)


def wire_frame(cur, prev, keyframe=False):
    """bitmap + payload + 1 XOR checksum byte, matching play_video.py."""
    if keyframe:
        bitmap = bytes([0xFF]) * BITMAP_BYTES
        payload = np.asarray(cur, dtype=np.uint8).tobytes()
    else:
        bitmap, payload = encode_delta(cur, prev)
    ck = 0
    for byt in bitmap:
        ck ^= byt
    if payload:
        ck ^= int(np.bitwise_xor.reduce(np.frombuffer(payload, dtype=np.uint8)))
    return bitmap, payload + bytes([ck])


def encode_delta(cur, prev):
    """(bitmap, payload) for the bytes that differ."""
    diff = cur != prev
    bitmap = np.packbits(diff)                     # 27200 bits -> 3400 bytes
    payload = cur[diff]
    return bitmap.tobytes(), payload.tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", help="directory of PNGs")
    ap.add_argument("out", nargs="?", help="output .bin (omit with --analyze)")
    ap.add_argument("--step", type=int, default=1, help="take every Nth frame")
    ap.add_argument("--limit", type=int, default=0, help="stop after N frames")
    ap.add_argument("--gamma", type=float, default=GAMMA)
    ap.add_argument("--contrast", type=float, default=CONTRAST)
    ap.add_argument("--dither", default="bayer4", choices=tuple(DITHERS),
                    help="cluster4 grows compact blobs instead of scattered single "
                         "pixels, which resolve far better at a short waveform")
    ap.add_argument("--rotate", type=int, default=0, choices=(0, 90, 270),
                    help="turn the video 90/270 deg and fit it to 272 px across the "
                         "panel's short side. Less active area, so less bandwidth")
    ap.add_argument("--keyframe", type=int, default=60,
                    help="every Nth frame is sent whole (0 = never). Insurance: a "
                         "delta stream cannot heal a dropped byte on its own")
    ap.add_argument("--analyze", action="store_true", help="report sizes, write nothing")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.frames, "*.png")))
    if not files:
        sys.exit("no PNGs found")
    files = files[:: args.step]
    if args.limit:
        files = files[: args.limit]

    lut = build_lut(args.gamma, args.contrast)
    if args.rotate:
        thresh = build_threshold(PORTRAIT_H, PORTRAIT_W, args.dither)
        xmap = portrait_x_map()
        print(f"rotated {args.rotate} deg: {PORTRAIT_W} px across, centred in "
              f"{PORTRAIT_H}")
    else:
        thresh = build_threshold(name=args.dither)
        xmap = None
    prev = np.full(PLANE_BYTES, 0xFF, dtype=np.uint8)  # panel starts white
    total_raw = total_enc = 0
    worst = 0
    fh = None
    if args.out and not args.analyze:
        fh = open(args.out, "wb")
        # Header: magic, frame count, w, h, plane bytes — so the player can sanity
        # check rather than trust the file blindly.
        fh.write(b"CP79")
        fh.write(len(files).to_bytes(4, "little"))
        fh.write(PANEL_W.to_bytes(2, "little"))
        fh.write(PANEL_H.to_bytes(2, "little"))
        fh.write(PLANE_BYTES.to_bytes(4, "little"))

    keys = 0
    for n, path in enumerate(files):
        if args.rotate:
            fb = frame_to_fb_rot(path, lut, thresh, xmap, args.rotate)
        else:
            fb = frame_to_fb(path, lut, thresh)
        # A KEYFRAME is just a frame whose bitmap is all ones, so the wire format
        # needs no flag for it: the decoder already overwrites every byte it is told
        # changed. Without these, one byte lost on the link stays wrong for the rest
        # of the clip, and in a column-major buffer that reads as a vertical stripe.
        if args.keyframe and n % args.keyframe == 0:
            bitmap = bytes([0xFF]) * BITMAP_BYTES
            payload = fb.tobytes()
            keys += 1
        else:
            bitmap, payload = encode_delta(fb, prev)
        # One XOR checksum byte per frame. The device verifies it and counts
        # mismatches, so a dropped byte on the link becomes a REPORTED number rather
        # than a visual guess — the protocol is otherwise blind to it, because the
        # popcount still yields the right byte COUNT even when the alignment slips.
        ck = 0
        for byt in bitmap:
            ck ^= byt
        ck ^= int(np.bitwise_xor.reduce(np.frombuffer(payload, dtype=np.uint8))) if payload else 0
        enc = len(bitmap) + len(payload) + 1
        total_raw += PLANE_BYTES
        total_enc += enc
        worst = max(worst, enc)
        if fh:
            fh.write(bitmap)
            fh.write(payload)
            fh.write(bytes([ck]))
        prev = fb
        if n % 25 == 0 or n == len(files) - 1:
            print(f"  {n+1}/{len(files)}  {enc} B  ({PLANE_BYTES/max(1,enc):.2f}x)",
                  flush=True)

    if fh:
        fh.close()

    n = len(files)
    avg = total_enc / n
    print()
    print(f"frames          {n}")
    print(f"raw             {PLANE_BYTES} B/frame")
    print(f"encoded avg     {avg:.0f} B/frame   ({total_raw/total_enc:.2f}x)")
    print(f"encoded worst   {worst} B/frame")
    print(f"keyframes       {keys}" if args.keyframe else "keyframes       none")
    print(f"total           {total_enc/1e6:.2f} MB")
    print()
    for fps in (20, 25, 30):
        need = avg * fps
        print(f"  {fps} fps needs {need/1000:.0f} kB/s  "
              f"= {need*10/1e6:.2f} Mbaud on a UART, "
              f"or {need/1e6*100:.1f}% of a 1 MB/s USB CDC link")
    if args.out and not args.analyze:
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
