"""How much smaller could each frame be? Host-side only, touches no hardware.

Measures encodings we could actually implement, at their true wire cost.
"""
import glob, os, sys, zlib
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from play import W, H, PLANE, BAYER4, build_threshold, frame_to_bits

folder = sys.argv[1] if len(sys.argv) > 1 else r"C:\clip\frames"
step = int(sys.argv[2]) if len(sys.argv) > 2 else 6

files = sorted(glob.glob(os.path.join(folder, "*.png")))[::step]
thresh = build_threshold(BAYER4, W, H)
frames = [frame_to_bits(f, thresh, False, 1.0).ravel() for f in files]
print(f"{len(frames)} frames, raw {PLANE} bytes each")


def rle_size(d):
    """(zero-run, literal-run, literals...) with 1-byte counts, 255 = continue."""
    nz = np.flatnonzero(d)
    if not len(nz):
        return 2
    total = 0
    pos = 0
    i = 0
    while i < len(nz):
        gap = nz[i] - pos
        j = i
        while j + 1 < len(nz) and nz[j + 1] == nz[j] + 1:
            j += 1
        lit = nz[j] - nz[i] + 1
        total += 1 + gap // 255          # zero-run count, 255-extended
        total += 1 + lit // 255 + lit    # literal count + the literals
        pos = nz[j] + 1
        i = j + 1
    return total


raw_tot = bmp_tot = rle_tot = zl_tot = xzl_tot = 0
changed_tot = 0
prev = np.full(PLANE, 0xFF, dtype=np.uint8)  # device starts from white
for f in frames:
    raw_tot += PLANE
    d = f ^ prev
    nchanged = int(np.count_nonzero(d))
    changed_tot += nchanged

    # 1. bitmap of changed bytes + the changed bytes themselves
    bmp_tot += PLANE // 8 + nchanged

    # 2. run-length over the XOR: skip zeros, copy literals
    rle_tot += rle_size(d)

    # 3/4. deflate, for reference
    zl_tot += len(zlib.compress(f.tobytes(), 6))
    xzl_tot += len(zlib.compress(d.tobytes(), 6))
    prev = f

n = len(frames)
print(f"changed bytes per frame: {changed_tot/n:.0f} of {PLANE} "
      f"({100*changed_tot/n/PLANE:.0f}%)\n")


def row(name, tot, note):
    print(f"  {name:<24} {tot/n:8.0f} B/f  {raw_tot/tot:5.2f}x  "
          f"{235000/(tot/n):5.1f} fps   {note}")


print("avg per frame, and the fps the 235 KB/s link would allow:")
row("raw (today)", raw_tot, "")
row("changed-byte bitmap", bmp_tot, "trivial decoder")
row("XOR + RLE", rle_tot, "trivial decoder")
row("XOR + deflate", xzl_tot, "needs inflate")
row("deflate alone", zl_tot, "needs inflate")
