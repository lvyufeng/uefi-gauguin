#!/usr/bin/env python3
"""Cut the a-priori batch down to its first N drivers.

The boot aborts with the same data abort (FAR=0xA59FE000, SP=0x9FFCD740) no matter which
driver is removed, so guessing the culprit one driver at a time is hopeless. Bisecting the
batch finds it in log2(N) round trips instead: keep the first N drivers, boot, and look at
whether the exception still happens.

    ./bisect-apriori.py 36        # keep the first 36 drivers
    ./bisect-apriori.py --all     # restore the whole batch

The original file is saved next to it as APRIORI.inc.orig on the first cut, and --all puts
it back, so the final tree never carries a half batch.
"""

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
APRIORI = HERE.parent / "Platforms/Xiaomi/gauguinPkg/Include/APRIORI.inc"
BACKUP = APRIORI.with_suffix(".inc.orig")

INF = re.compile(r"^\s*INF\s+\S+\s*$")


def driver_lines(text):
    """Indices of the lines that actually add a driver to the batch."""
    return [i for i, line in enumerate(text.splitlines()) if INF.match(line)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("count", nargs="?", type=int, help="number of drivers to keep")
    ap.add_argument("--all", action="store_true", help="restore the full batch")
    args = ap.parse_args()

    if args.all:
        if not BACKUP.is_file():
            sys.exit(f"no backup at {BACKUP}; nothing to restore")
        APRIORI.write_text(BACKUP.read_text())
        print(f"restored {len(driver_lines(BACKUP.read_text()))} drivers from {BACKUP.name}")
        return

    if args.count is None:
        sys.exit("give a driver count, or --all")

    if not BACKUP.is_file():
        BACKUP.write_text(APRIORI.read_text())
        print(f"saved the full batch to {BACKUP.name}")

    text = BACKUP.read_text()
    lines = text.splitlines()
    keep = driver_lines(text)
    if not 0 <= args.count <= len(keep):
        sys.exit(f"count must be 0..{len(keep)}")

    cut = set(keep[args.count:])
    out = []
    for i, line in enumerate(lines):
        if i in cut:
            out.append("  # " + line.strip() + "   [bisect: dropped]")
        else:
            out.append(line)
    APRIORI.write_text("\n".join(out) + "\n")
    print(f"kept {args.count} of {len(keep)} drivers, dropped {len(cut)}")


if __name__ == "__main__":
    main()