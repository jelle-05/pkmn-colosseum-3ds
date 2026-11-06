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
    ap.add_argument("--gdb", action="store_true",
                    help="enable Azahar's gdbstub, stop at the first heartbeat "
                         "(__port_thread_dump) and print every thread's backtrace")
    args = ap.parse_args()

    STUB_LOG.unlink(missing_ok=True)
    started = time.time()
    config = APPDATA / "Azahar" / "config" / "qt-config.ini"
    saved = config.read_text(encoding="utf-8") if args.gdb else None
    if args.gdb:
        config.write_text(saved.replace("use_gdbstub=false", "use_gdbstub=true")
                          .replace("use_gdbstub\\default=true", "use_gdbstub\\default=false"), encoding="utf-8")
    proc = subprocess.Popen([args.azahar, str(args.rom.resolve())])
    try:
        if args.gdb:
            time.sleep(3)
            elf = args.rom.with_suffix(".elf").resolve().as_posix()
            gdb = Path(os.environ.get("DEVKITARM_WIN", r"C:\devkitPro\devkitARM")) / "bin" / "arm-none-eabi-gdb.exe"
            cmds = ["set pagination off", "set confirm off", f"file {elf}",
                    "target remote localhost:24689", "break __port_thread_dump", "continue",
                    "info threads", "thread apply all bt 12", "detach"]
            p = subprocess.run([str(gdb), "-batch", *sum((["-ex", c] for c in cmds), [])],
                               capture_output=True, text=True, timeout=args.seconds + 60)
            out = ROOT / "build" / "gdb.txt"
            out.write_text(p.stdout + p.stderr, encoding="utf-8")
            print(p.stdout[-6000:])
            print(p.stderr[-1500:])
        else:
            time.sleep(args.seconds)
    finally:
        proc.kill()
        proc.wait()
        if saved is not None:
            config.write_text(saved, encoding="utf-8")

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
