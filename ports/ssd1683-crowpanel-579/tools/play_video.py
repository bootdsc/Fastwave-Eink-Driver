"""Stream video to the CrowPanel 5.79.

Pre-encoded clip (old path):

    python tools/play_video.py COM17 bbb_c8.bin --drive 4 --old 1 --settle 1 --fps 12 --loop

Any video file, dithered live (file dialog if you omit the path):

    python tools/play_video.py COM17
    python tools/play_video.py COM17 D:\\path\\clip.mp4 --loop

The device is spoken to at 115200 for commands, then BOTH ends switch to 2 Mbaud
for the stream and back again at the end. The command channel is deliberately left
at 115200: a board locked to 2 Mbaud is a board you cannot talk to if that rate
turns out to be unreliable on a given cable.

Flow control is one ACK byte (0x06) per chunk, sent BY THE DEVICE - it asks for the
next 4096 bytes when it has room. That means the host can never overrun the CDC
receive queue no matter how fast it writes.

Measured on Big Buck Bunny at 792x272: 6837 B/frame average, so 25 fps needs
171 kB/s = 1.71 Mbaud. 2 Mbaud gives ~17% headroom.
"""

import argparse
import os
import struct
import sys
import time

import numpy as np
import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import encode_video as enc

CMD_BAUD = 115200
STREAM_BAUD = 2000000
CHUNK = 12288   # must match STREAM_CHUNK in the firmware
ACK = 0x06


def show(b):
    """Print device output safely. The Windows console is cp1252 and the device
    emits UTF-8 em-dashes, which raise UnicodeEncodeError on print()."""
    if isinstance(b, bytes):
        b = b.decode("utf-8", "replace")
    sys.stdout.write(b.encode("ascii", "replace").decode("ascii"))
    sys.stdout.write("\n")
    sys.stdout.flush()


def read_header(fh):
    magic = fh.read(4)
    if magic != b"CP79":
        sys.exit(f"not a CP79 file (magic {magic!r}) - re-run encode_video.py")
    nframes, w, h, plane = struct.unpack("<IHHI", fh.read(12))
    return nframes, w, h, plane


def send_chunked(s, data, timeout=5.0):
    """Send `data` in CHUNK pieces, one device ACK per piece."""
    off = 0
    while off < len(data):
        t0 = time.time()
        while True:
            b = s.read(1)
            if b and b[0] == ACK:
                break
            if time.time() - t0 > timeout:
                return False
        n = min(CHUNK, len(data) - off)
        s.write(data[off : off + n])
        off += n
    return True


def reenter_stream(s):
    """Send `stream`, wait for the banner, then switch both ends to STREAM_BAUD."""
    s.baudrate = CMD_BAUD
    time.sleep(0.1)
    s.reset_input_buffer()
    s.write(b"stream\n")
    t0 = time.time()
    banner = b""
    while time.time() - t0 < 6:
        banner += s.read(256)
        if b"STREAM ready" in banner:
            break
    if b"STREAM ready" not in banner:
        show("device did not re-enter stream mode: " + repr(banner))
        return False
    show(banner.decode("utf-8", "replace").strip())
    time.sleep(0.05)
    s.baudrate = STREAM_BAUD
    time.sleep(0.15)
    # Drop the rate-change garbage plus any acks sent while we were still slow; the
    # device repeats them every 150 ms until data flows.
    s.reset_input_buffer()
    return True


def is_cp79(path):
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == b"CP79"
    except OSError:
        return False


def pick_video():
    try:
        import tkinter as tk
        from tkinter import filedialog
    except ImportError:
        sys.exit("no video path given, and tkinter is missing")
    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    path = filedialog.askopenfilename(
        parent=root,
        title="Video for CrowPanel",
        filetypes=[
            ("Video", "*.mp4 *.mkv *.mov *.avi *.webm *.m4v"),
            ("PNG sequence folder — pick any png", "*.png"),
            ("Pre-encoded CP79", "*.bin"),
            ("All files", "*.*"),
        ],
    )
    root.destroy()
    return path


def bgr_to_pil(bgr):
    from PIL import Image
    rgb = bgr[:, :, ::-1]
    return Image.fromarray(rgb)


def play_live(s, path, args, send_one, period):
    try:
        import cv2
    except ImportError:
        sys.exit("pip install opencv-python")

    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        sys.exit("cannot open video: " + path)

    src_fps = cap.get(cv2.CAP_PROP_FPS) or 0.0
    nsrc = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    target = args.fps
    step = max(1, int(round(src_fps / target))) if src_fps > 1 else 1
    rot = args.rotate
    lut = enc.build_lut(args.gamma, args.contrast)
    if rot:
        thresh = enc.build_threshold(enc.PORTRAIT_H, enc.PORTRAIT_W, args.dither)
        xmap = enc.portrait_x_map()
    else:
        thresh = enc.build_threshold(name=args.dither)
        xmap = None

    print("live %s  %g fps src  step=%d  -> %g fps  rotate=%d  %s  c=%.2f g=%.2f" %
          (path, src_fps, step, target, rot, args.dither, args.contrast, args.gamma))
    if nsrc:
        print("  %d source frames" % nsrc)

    keyframe = max(0, args.keyframe)
    prev = np.full(enc.PLANE_BYTES, 0xFF, dtype=np.uint8)
    sent = 0
    start = time.time()
    src_i = 0
    restart = False
    while True:
        ok, bgr = cap.read()
        if (not ok or bgr is None) and args.loop:
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            src_i = 0
            ok, bgr = cap.read()
        if not ok or bgr is None:
            break
        keep = (src_i % step == 0)
        src_i += 1
        if not keep:
            continue
        im = bgr_to_pil(bgr)
        if rot:
            fb = enc.image_to_fb_rot(im, lut, thresh, xmap, rot)
        else:
            fb = enc.image_to_fb(im, lut, thresh)
        kf = bool(keyframe and (sent % keyframe == 0))
        bmp, pay = enc.wire_frame(fb, prev, keyframe=kf)
        prev = fb
        if not send_one(bmp, pay):
            restart = True
            break
        sent += 1
        if period:
            due = start + sent * period
            now = time.time()
            if due > now:
                time.sleep(due - now)
        if sent % 25 == 0:
            el = max(time.time() - start, 0.001)
            print("\r  %d frames  %5.1f fps" % (sent, sent / el), end="", flush=True)

    cap.release()
    return sent, restart


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("file", nargs="?", default="",
                    help="mp4/mkv/mov, or a CP79 .bin from encode_video.py. "
                         "Omit to pick a file.")
    ap.add_argument("--fps", type=float, default=None, help="pacing target")
    ap.add_argument("--no-pace", action="store_true", help="send as fast as accepted")
    ap.add_argument("--drive", type=int, default=0, help="set the panel mode first")
    ap.add_argument("--drives", default="",
                    help="comma list, e.g. 3,4,5,6 - plays the whole clip once at each "
                         "so the modes can be compared back to back in one sitting")
    # Both of these must be set in THIS session: the board resets when the port
    # closes, so anything set by another tool is gone before the stream starts.
    ap.add_argument("--old", type=int, default=None, choices=(0, 1, 2, 3),
                    help="OLD-plane strategy. 0=skip (fastest, but abandons "
                         "part-finished transitions), 1=true diff, 2=false diff "
                         "(drives EVERY pixel every frame - best for video), 3=trail")
    ap.add_argument("--trail", type=int, default=None,
                    help="frames of re-drive per change; only meaningful with --old 3")
    ap.add_argument("--settle", type=int, default=None,
                    help="VSS hold frames after the drive. Removed for speed, but on "
                         "video every pixel is driven back to back with no idle time, "
                         "which is exactly when an unrelaxed stack could carry over")
    ap.add_argument("--repeat", type=int, default=1,
                    help="play the clip N times back to back within one stream session")
    ap.add_argument("--rotate", type=int, default=90, choices=(0, 90, 270),
                    help="live video only. 90 = standing CrowPanel letterbox")
    ap.add_argument("--dither", default="cluster8", choices=tuple(enc.DITHERS))
    ap.add_argument("--gamma", type=float, default=enc.GAMMA)
    ap.add_argument("--contrast", type=float, default=1.8)
    ap.add_argument("--keyframe", type=int, default=60)
    ap.add_argument("--loop", dest="loop", action="store_true", default=None)
    ap.add_argument("--once", dest="loop", action="store_false")
    args = ap.parse_args()

    path = args.file.strip().strip('"')
    if not path:
        path = pick_video()
        if not path:
            sys.exit("no file picked")
    args.file = path

    live = not is_cp79(path)
    if args.loop is None:
        args.loop = live
    if live:
        if args.fps is None:
            args.fps = 12.0
        if not args.drive:
            args.drive = 4
        if args.old is None:
            args.old = 1
        if args.settle is None:
            args.settle = 1
    else:
        if args.fps is None:
            args.fps = 25.0

    fh = None
    nframes = w = h = plane = bmp_bytes = 0
    if not live:
        fh = open(args.file, "rb")
        nframes, w, h, plane = read_header(fh)
        bmp_bytes = plane // 8
        print(f"{args.file}: {nframes} frames, {w}x{h}, plane {plane} B, bitmap {bmp_bytes} B")
    else:
        print("live file: " + args.file)

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, CMD_BAUD, 0.2
    s.write_timeout = 10
    # Do NOT reset the board on attach: it keeps its image and its voltage settings
    # across a reconnect but not across a reset.
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()

    def set_drive(d):
        # Must happen in THIS session: the board resets when the port closes, so a
        # mode set by any other tool is gone before the stream starts.
        s.baudrate = CMD_BAUD
        s.reset_input_buffer()
        s.write(("drive " + str(d) + "\n").encode())
        # Two GC clears plus a 32-frame bar sweep. Wait for the ack rather than
        # guessing, then drain whatever the sweep logged.
        t0 = time.time()
        buf = b""
        while time.time() - t0 < 15:
            buf += s.read(256)
            if b"OK drive" in buf:
                break
        show("--- mode " + str(d) + " ---")
        show(buf.decode("utf-8", "replace").strip()[-160:])
        time.sleep(0.3)
        s.reset_input_buffer()

    def set_opts():
        # Sent BEFORE `drive N`, not after.
        #
        # `settle` is a waveform parameter, so it has to be in place before the arm or
        # the banner reports a frame count the stream will not actually run at. It did
        # still take effect afterwards — set_wave_byte re-arms — but the printed
        # predicted_ms was stale, which is exactly the kind of thing that makes an A/B
        # look invalid when it was fine.
        for opt, val in (("old", args.old), ("trail", args.trail),
                         ("settle", args.settle)):
            if val is None:
                continue
            s.write((opt + " " + str(val) + "\n").encode())
            time.sleep(0.4)
            show(s.read(512).decode("utf-8", "replace").strip())

    drives = [int(x) for x in args.drives.split(",") if x.strip()]
    if not drives and args.drive:
        drives = [args.drive]
    di = 0
    set_opts()
    if drives:
        set_drive(drives[0])

    if not reenter_stream(s):
        sys.exit("device did not enter stream mode")

    period = 1.0 / args.fps if not args.no_pace else 0.0
    total_sent = 0
    try:
        # Outer loop: one pass per stream session. A button press on the panel makes
        # the device LEAVE stream mode (it cannot resynchronise a half-received frame
        # safely), clear, and announce the new mode — so we simply re-enter and
        # replay from frame 0, which is what its first delta is encoded against.
        while True:
            sent = 0
            start = time.time()
            restart = False
            # --repeat plays the clip again WITHOUT leaving stream mode, so the passes
            # run truly back to back. Going round the outer loop instead would insert
            # a mode change, i.e. two GC clears and a bar sweep, between every pass.
            def send_one(bmp, pay):
                return send_chunked(s, bmp) and send_chunked(s, pay)

            if live:
                sent, restart = play_live(s, args.file, args, send_one, period)
                total_sent += sent
            else:
                for _rep in range(max(1, args.repeat)):
                    fh.seek(16)
                    for _ in range(nframes):
                        bmp = fh.read(bmp_bytes)
                        if len(bmp) < bmp_bytes:
                            break
                        npay = sum(bin(b).count("1") for b in bmp)
                        pay = fh.read(npay + 1)   # payload plus its checksum byte
                        if not send_one(bmp, pay):
                            restart = True     # device left stream mode, or stalled
                            break
                        sent += 1
                        total_sent += 1
                        if period:
                            # Pace against the wall clock, not per-frame sleeps, so a
                            # slow frame does not push every later one late.
                            target = start + sent * period
                            now = time.time()
                            if target > now:
                                time.sleep(target - now)
                        if sent % 25 == 0:
                            el = time.time() - start
                            print(f"\r  {sent} frames  {sent/el:5.1f} fps",
                                  end="", flush=True)
                    if restart:
                        break

            el = time.time() - start
            print(f"\rsent {sent} frames in {el:.1f} s = {sent/max(el,0.001):.1f} fps    ")

            # Drop to the command rate and wait for the device to report.
            #
            # This has to POLL, not sleep once: on a button press the device first
            # waits for its receive task to exit (up to 1.5 s), then does a full GC
            # clear, and only then prints. A single short sleep missed the message
            # entirely, which is why a mode change appeared to just abandon the video.
            s.baudrate = CMD_BAUD
            msg = ""
            t0 = time.time()
            while time.time() - t0 < 9:
                msg += s.read(8192).decode("utf-8", "replace")
                if "STREAM restart" in msg or "STREAM done" in msg:
                    break
            show(msg.strip())

            if "STREAM restart" in msg:
                if not reenter_stream(s):
                    break
                continue
            if restart and "STREAM" not in msg:
                print("device stopped acking and said nothing - giving up")
                break

            # Next mode in the sweep, if there is one. Each gets the whole clip so
            # they can be judged against each other rather than from memory.
            di += 1
            if di < len(drives):
                set_opts()
                set_drive(drives[di])
                if not reenter_stream(s):
                    break
                continue

            if args.loop:
                if not reenter_stream(s):
                    break
                continue
            break
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        print(f"total {total_sent} frames")
        s.baudrate = CMD_BAUD
        time.sleep(0.5)
        s.reset_input_buffer()
        s.close()
        if fh is not None:
            fh.close()


if __name__ == "__main__":
    main()
