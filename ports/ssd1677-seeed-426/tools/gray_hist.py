"""Level histogram for --gray, to see whether an image is genuinely too bright."""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import play

path = sys.argv[1]
thr = (play.BAYER4 + 0.5) / play.BAYER4.size
thr = np.tile(thr, (play.H // 4 + 1, play.W // 4 + 1))[:play.H, :play.W]

print(f"{'gamma':>6} {'contr':>6}   code0(blk)  code1  code2  code3(wht)")
for gamma, contrast in [(1.0, 1.0), (1.2, 1.4), (1.55, 1.4), (1.8, 1.4),
                        (2.2, 1.4), (1.8, 1.8)]:
    low, high = play.to_gray_planes(path, False, gamma, contrast, thr)
    lo = np.unpackbits(low).astype(np.uint8)
    hi = np.unpackbits(high).astype(np.uint8)
    code = (hi << 1) | lo
    c = np.bincount(code, minlength=4) / code.size * 100
    print(f"{gamma:6.2f} {contrast:6.2f}   "
          + "  ".join(f"{v:8.1f}%" for v in c))
