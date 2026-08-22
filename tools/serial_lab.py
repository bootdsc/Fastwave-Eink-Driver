import sys, time, serial

# usage: bench.py COM18 LOGFILE cmd1 cmd2 ...    ("@N" = wait N seconds)
port, logpath = sys.argv[1], sys.argv[2]
cmds = sys.argv[3:]

log = open(logpath, 'w', encoding='utf-8', buffering=1)
# Opening the port normally asserts DTR/RTS, which hard-resets the ESP32 and
# destroys whatever the user was in the middle of looking at. Deassert both
# BEFORE opening so attaching a monitor is invisible to the board.
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()
t0 = time.time()
pending = b''


def drain(secs):
    """Hard wall-clock cap. Never extends — a chatty board must not be able to
    stop commands from being sent."""
    global pending
    end = time.time() + secs
    while time.time() < end:
        d = s.read(4096)
        if not d:
            continue
        pending += d
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            log.write('%7.2f %s\n' % (time.time() - t0, line.decode('utf-8', 'replace').rstrip('\r')))


drain(3.0)
for c in cmds:
    if c.startswith('@'):
        drain(float(c[1:]))
        continue
    log.write('%7.2f >>> %s\n' % (time.time() - t0, c))
    s.write((c + '\n').encode())
    s.flush()
    drain(4.0)
s.close()
log.write('*** done\n')
log.close()
print('done')
