"""Read whatever the board is emitting, optionally sending a command first."""
import serial, sys, time

port = sys.argv[1]
cmd = sys.argv[2] if len(sys.argv) > 2 else None
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.3
s.dtr = s.rts = False
s.open()
time.sleep(0.3)
s.reset_input_buffer()
if cmd:
    s.write((cmd + "\n").encode())
buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    buf += s.read(4096)
s.close()
print(f"{len(buf)} bytes")
print(buf.decode("utf-8", "replace")[-1500:])
