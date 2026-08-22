"""Flash a fastwave port environment.

  python flash.py COM30 xiao-154   # Watchy 1.54" 200x200 SSD1681
  python flash.py COM19 xiao-426   # Seeed 4.26" 800x480 SSD1677

The env is REQUIRED. It used to default to xiao-426, and a defaulted env is
exactly how a 48000-byte plane, an X window of 0..799 and a gate count of 479
ended up pushed into a 5000-byte, 200-gate controller for a whole session. The
glass on the bench decides the env; nothing here can infer it.

1) Ask the app to jump to ROM (`download`) if CDC is alive.
2) Else 1200-bps touch (Arduino TinyUSB reset-to-bootloader).
3) platformio upload.

If the device has a failed descriptor, hold BOOT, tap RESET, then run this.
"""
import os
import re
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTS = os.path.join(REPO, "ports")


def env_map():
    """{env name: the port directory whose platformio.ini declares it}.

    Every port owns its own build environment, and the environment name is how
    this tool knows which project to hand to platformio. That mapping is read
    off disk rather than hardcoded, so a port added later needs no edit here.
    """
    out = {}
    for entry in sorted(os.listdir(PORTS)) if os.path.isdir(PORTS) else []:
        ini = os.path.join(PORTS, entry, "platformio.ini")
        try:
            with open(ini, "r", encoding="utf-8") as fh:
                for name in re.findall(r"^\[env:([^\]]+)\]", fh.read(), re.M):
                    out[name] = os.path.join(PORTS, entry)
        except OSError:
            continue
    return out


def ports():
    return {p.device.upper(): p for p in serial.tools.list_ports.comports()}


def have(name):
    return name.upper() in ports()


def report_flashed(port):
    """Print which env is on the board now. Informational -- never a default."""
    if not have(port):
        return
    try:
        s = serial.Serial()
        s.port, s.baudrate = port, 115200
        s.timeout = s.write_timeout = 1.5
        s.dtr = s.rts = False
        s.open()
    except Exception:
        return
    try:
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(b"status\n")
        s.flush()
        raw = s.read(400)
        m = re.search(rb"STATUS[^\r\n]*", raw or b"")
        if m:
            print("on board now: %s" % m.group(0).decode("utf-8", "replace")[:150])
    except Exception:
        pass
    finally:
        try:
            s.close()
        except Exception:
            pass


def send_download(port):
    if not have(port):
        return False
    try:
        s = serial.Serial()
        s.port, s.baudrate = port, 115200
        s.timeout = s.write_timeout = 1
        s.dtr = s.rts = False
        s.open()
        time.sleep(0.15)
        s.write(b"download\n")
        s.flush()
        time.sleep(0.15)
        s.close()
        print("sent download")
        return True
    except Exception as e:
        print("download cmd failed:", e)
        return False


def touch_1200(port):
    """Arduino native-USB: open 1200 baud then close -> ROM bootloader."""
    if not have(port):
        return False
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 1200
        s.timeout = 0.3
        s.dtr = False
        s.rts = False
        s.open()
        time.sleep(0.3)
        s.close()
        print("1200-bps touch")
        return True
    except Exception as e:
        print("1200-bps touch failed:", e)
        return False


def wait_port(port, secs=20):
    t0 = time.time()
    while time.time() - t0 < secs:
        if have(port):
            return True
        time.sleep(0.25)
    return False


def main():
    envs = env_map()
    if len(sys.argv) < 3:
        sys.exit("usage: flash.py PORT ENV\n  envs: %s\n"
                 "  The glass on the bench decides the env. No default."
                 % (", ".join(sorted(envs)) or "see ports/*/platformio.ini"))
    port, env = sys.argv[1], sys.argv[2]
    if env not in envs:
        sys.exit("unknown env %r - ports/*/platformio.ini declare: %s"
                 % (env, ", ".join(sorted(envs))))
    proj = envs[env]

    print("flashing %s -> %s" % (env, port))
    report_flashed(port)

    if have(port):
        send_download(port)
        time.sleep(0.8)
        if have(port):
            touch_1200(port)
            time.sleep(1.2)
    else:
        print(port, "not listed — if descriptor failed, BOOT+RESET now")

    if not wait_port(port, 20):
        sys.exit("no " + port + " — hold BOOT, tap RESET, rerun")

    # platformio echoes esptool's box-drawing output through click; on a cp1252
    # console that raises UnicodeEncodeError mid-upload and buries a successful
    # flash in a traceback.
    sub_env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.call(
        [
            sys.executable,
            "-m",
            "platformio",
            "run",
            "-e",
            env,
            "-t",
            "upload",
            "--upload-port",
            port,
        ],
        cwd=proj,
        env=sub_env,
    )
    sys.exit(r)


if __name__ == "__main__":
    main()
