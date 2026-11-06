#!/usr/bin/env python3
"""Run build/link_probe/colosseum.3dsx in Azahar for a while and report.

Starts Azahar with the probe build, waits, closes it, and prints:

  - the stub log the port writes to Azahar's SD card (the first
    unimplemented symbol the game reached, or a bad fixed address);
  - unmapped memory accesses from Azahar's log. Azahar tolerates them, real
    hardware does not, so any count above zero is a bug;
  - debug output (OSReport) and errors from Azahar's log.

Usage:
    python tools/run_azahar.py [--seconds 15] [--azahar PATH]
"""

import argparse
import os
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPDATA = Path(os.environ.get("APPDATA", ""))
STUB_LOG = APPDATA / "Azahar" / "sdmc" / "colosseum-port-stub.log"
AZ_LOG = APPDATA / "Azahar" / "log" / "azahar_log.txt"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--azahar", default=r"C:\Program Files\Azahar\azahar.exe")
    ap.add_argument("--rom", type=Path, default=ROOT / "build" / "link_probe" / "colosseum.3dsx")
    args = ap.parse_args()

    STUB_LOG.unlink(missing_ok=True)
    started = time.time()
    proc = subprocess.Popen([args.azahar, str(args.rom.resolve())])
    try:
        time.sleep(args.seconds)
    finally:
        proc.kill()
        proc.wait()

    if not AZ_LOG.exists() or AZ_LOG.stat().st_mtime < started:
        print("Azahar did not write a new log")
    log = AZ_LOG.read_text(encoding="utf-8", errors="replace").splitlines() if AZ_LOG.exists() else []

    print("stub log:")
    if STUB_LOG.exists():
        for line in STUB_LOG.read_text(encoding="utf-8", errors="replace").splitlines():
            print("   ", line)
    else:
        print("    (none: no stub reached)")

    unmapped = [l for l in log if "unmapped" in l.lower()]
    print(f"unmapped accesses: {len(unmapped)}")
    for l in unmapped[:5]:
        print("   ", l.split("UnmappedAccess:", 1)[-1].strip()[:140])

    debug = [l for l in log if "Debug.Emulated" in l or "OutputDebugString" in l]
    print(f"debug output lines: {len(debug)}")
    for l in debug[-15:]:
        print("   ", re.sub(r"^\[[^\]]*\]\s*", "", l)[:160])

    errors = [l for l in log if "<Critical>" in l or "<Error>" in l]
    errors = [l for l in errors if "unmapped" not in l.lower()]
    print(f"other errors: {len(errors)}")
    for l in errors[:8]:
        print("   ", re.sub(r"^\[[^\]]*\]\s*", "", l)[:160])


if __name__ == "__main__":
    main()
