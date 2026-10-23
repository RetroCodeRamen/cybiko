#!/usr/bin/env python3
# Usage: capture_serial.py <seconds> <logfile>   (pyserial; ~/.platformio/penv/bin/python has it)
# Reads /dev/ttyACM0 (reopening across USB re-enumeration) and timestamps lines.
import sys, time, serial

dur = float(sys.argv[1])
out = open(sys.argv[2], "w")
start = time.time()
port = None
buf = b""
while time.time() - start < dur:
    if port is None:
        try:
            port = serial.Serial("/dev/ttyACM0", 115200, timeout=0.5)
        except Exception:
            time.sleep(0.2)
            continue
    try:
        data = port.read(4096)
    except Exception:
        port = None
        continue
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = "%6.1f %s" % (time.time() - start, line.decode("utf-8", "replace").rstrip())
        print(s, flush=True)
        out.write(s + "\n")
        out.flush()
