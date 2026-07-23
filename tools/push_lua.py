#!/usr/bin/env python3
"""Push a file onto the device's LittleFS over the T: serial harness —
no uploadfs, no data-partition wipe (identity/WiFi/prefs survive).

Usage:
  python3 tools/push_lua.py <port> <local_file> <device_path> [more pairs...]
  python3 tools/push_lua.py /dev/cu.usbmodem101 data/lua/lib/rns.lua /lua/lib/rns.lua

Requires a HYBRID_TEST_HOOKS build (env meshpunk_test) on the device.
Uses pyserial (bundled with PlatformIO's python; else `pip install pyserial`).
"""
import base64
import sys
import time

import serial  # pyserial

# Decoded bytes per T:FDATA line. The device's serial line assembler
# buffers 768 chars ("T:FDATA <seq> " + b64); 384 decoded = 512 b64
# chars, comfortable margin. Do not raise past the device buffer.
CHUNK = 384


def wait_ok(ser, what, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("T:OK"):
            return line
        if line.startswith("T:ERR"):
            raise SystemExit(f"device error during {what}: {line}")
        # anything else is unrelated log traffic — keep scanning
    raise SystemExit(f"timeout waiting for T:OK during {what}")


def push(ser, local, remote):
    with open(local, "rb") as f:
        data = f.read()
    ser.write(f"T:FOPEN {remote}\n".encode())
    wait_ok(ser, f"FOPEN {remote}")
    seq = 0
    for i in range(0, len(data), CHUNK):
        b64 = base64.b64encode(data[i:i + CHUNK]).decode()
        # Retry on lost replies — seq makes resends idempotent on the
        # device (an already-applied chunk is re-ACKed, not re-written).
        for attempt in range(4):
            ser.write(f"T:FDATA {seq} {b64}\n".encode())
            try:
                wait_ok(ser, f"FDATA seq={seq}")
                break
            except SystemExit:
                if attempt == 3:
                    raise
                time.sleep(0.5)
        seq += 1
    ser.write(b"T:FCLOSE\n")
    reply = wait_ok(ser, "FCLOSE")
    size = int(reply.split("size=")[1]) if "size=" in reply else -1
    if size != len(data):
        raise SystemExit(f"{remote}: device wrote {size}B, expected {len(data)}B")
    print(f"pushed {local} -> {remote} ({size}B)")


def main():
    if len(sys.argv) < 4 or (len(sys.argv) - 2) % 2 != 0:
        raise SystemExit(__doc__)
    port = sys.argv[1]
    pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        time.sleep(0.3)          # let the port settle; do NOT reset the board
        ser.reset_input_buffer()
        for local, remote in pairs:
            push(ser, local, remote)
    print("done — relaunch the app (or reboot) to pick the files up")


if __name__ == "__main__":
    main()
