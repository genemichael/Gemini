#!/usr/bin/env python3
"""Send T:-harness commands to the device and print tagged replies.

Usage: python3 tools/tsh.py <port> "<cmd>" ["<cmd>" ...]
Each command is sent after the previous one's terminal T:OK/T:ERR (or a
2s quiet timeout); all T:-prefixed lines are printed, other log noise
suppressed unless -v.
"""
import sys
import time

import serial

VERBOSE = "-v" in sys.argv
args = [a for a in sys.argv[1:] if a != "-v"]
if len(args) < 2:
    raise SystemExit(__doc__)
port, cmds = args[0], args[1:]

with serial.Serial(port, 115200, timeout=0.25) as ser:
    time.sleep(0.3)
    ser.reset_input_buffer()
    for cmd in cmds:
        print(f">>> {cmd}")
        ser.write((cmd + "\n").encode())
        deadline = time.time() + 6.0
        quiet = time.time() + 2.0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                if time.time() > quiet:
                    break
                continue
            quiet = time.time() + 2.0
            if line.startswith("T:"):
                print(line)
                if line.startswith("T:OK") or line.startswith("T:ERR"):
                    # count= replies stream extra lines after the OK
                    if "count=" not in line:
                        break
            elif VERBOSE:
                print("  | " + line)
