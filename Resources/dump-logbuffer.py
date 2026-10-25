#!/usr/bin/env python3
"""Pull the Log Buffer off a phone that is sitting in recovery.

The firmware mirrors every driver start, the last successful driver, and the register state at a
fault into the reserved 32 KiB region at 0x9FFF7000. That region is EfiRuntimeServicesData, so a
reboot into recovery leaves the contents intact; this reads it back through adb and prints it in
order, wrapping as the ring buffer does.

    ./dump-logbuffer.py              # decode what is already dumped
    ./dump-logbuffer.py --pull       # adb shell dd it off the phone first

The address has to match the "Log Buffer" entry in MemoryMapLib.c.
"""

import argparse
import pathlib
import struct
import subprocess
import sys

LOG_BASE = 0x9FFF7000
LOG_SIZE = 0x00008000
MAGIC = 0x474E554C47554C55
HEADER = struct.Struct("<QIIII")  # magic, version, head, total, capacity

DEVICE_PATH = "/dev/mem_lb"
LOCAL = pathlib.Path("/tmp/logbuffer.bin")


def pull():
    """Copy the region out. recovery's /dev/mem has to be seekable, hence dd with a skip."""
    cmd = f"dd if=/dev/mem of={DEVICE_PATH} bs=4096 skip={LOG_BASE // 4096} count={LOG_SIZE // 4096} 2>/dev/null"
    print(f"adb shell {cmd}")
    subprocess.run(["adb", "shell", cmd], check=True)
    subprocess.run(["adb", "pull", DEVICE_PATH, str(LOCAL)], check=True)


def decode(data):
    if len(data) < HEADER.size:
        sys.exit(f"dump is only {len(data)} bytes, too short for a header")

    magic, version, head, total, capacity = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        sys.exit(
            f"no Log Buffer at 0x{LOG_BASE:X}: magic is 0x{magic:016X}, expected 0x{MAGIC:016X}.\n"
            "Either the firmware never wrote to it, or the phone has been power cycled since \
(the region is DRAM and does not survive that)."
        )

    text = data[HEADER.size:HEADER.size + capacity]
    if head == 0 and total == 0:
        print("buffer is empty: the firmware initialised it but nothing was logged")
        return
    print(f"# {total} bytes written, head at {head} of {capacity}, version {version}")

    # Unwrap: the oldest byte is at head when full, otherwise at 0.
    if total <= capacity:
        ordered = text[:head]
    else:
        ordered = text[head:] + text[:head]

    sys.stdout.write(ordered.decode("ascii", "replace"))
    if ordered and not ordered.endswith(b"\n"):
        sys.stdout.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pull", action="store_true", help="copy the region off the phone first")
    ap.add_argument("--file", help="decode this dump instead of the one pulled from the phone")
    args = ap.parse_args()

    if args.pull:
        pull()

    path = pathlib.Path(args.file) if args.file else LOCAL
    if not path.is_file():
        sys.exit(f"no dump at {path}; run with --pull while the phone is in recovery")
    decode(path.read_bytes())


if __name__ == "__main__":
    main()