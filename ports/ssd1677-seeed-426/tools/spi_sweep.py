"""Find the highest SPI clock the controller still accepts commands at.

spi_ms shows the transfer getting faster; busy_ms shows whether the panel
actually refreshed. A clock that is too fast gives a fast transfer and
busy_ms=0 — the bytes go out, the controller does not act on them.
"""
import re, sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM19"
speeds = [int(v) for v in sys.argv[2].split(",")] if len(sys.argv) > 2 else \
    [20, 24, 26, 28, 30, 32, 36, 40]

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.3
s.dtr = s.rts = False
s.open()
time.sleep(0.3)
s.write(b"run\n")
time.sleep(0.5)

print(f"{'MHz':>4} {'spi_ms':>7} {'busy_ms':>8}   verdict")
for mhz in speeds:
    s.reset_input_buffer()
    s.write(f"spi hw {mhz}\n".encode())
    time.sleep(3.0)
    txt = s.read(20000).decode("utf-8", "replace")
    rows = re.findall(r"spi_ms=(\d+) busy_ms=(\d+)", txt)
    if not rows:
        print(f"{mhz:>4} {'-':>7} {'-':>8}   no response")
        continue
    spi = sum(int(a) for a, _ in rows) / len(rows)
    busy = sum(int(b) for _, b in rows) / len(rows)
    ok = "OK" if busy > 10 else "BROKEN - panel not refreshing"
    print(f"{mhz:>4} {spi:7.1f} {busy:8.1f}   {ok}")

s.write(b"spi hw 20\n")
time.sleep(0.5)
s.write(b"stop\n")
s.close()
print("\nrestored to 20 MHz")
