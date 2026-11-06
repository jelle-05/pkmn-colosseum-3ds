#!/usr/bin/env python3
"""Run build/port/colosseum.3dsx in Azahar for a while and report.

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


def heartbeat_address(image, tools):
    out = subprocess.run([str(tools / "arm-none-eabi-nm.exe"), str(image)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == "__port_thread_dump":
            return "0x" + p[0]
    raise SystemExit("__port_thread_dump not in the image")


def resolve_registers(text, elfs, tools):
    """Function names for every pc/lr value gdb printed."""
    values = sorted(set(re.findall(r"^(?:pc|lr)\s+(0x[0-9a-f]+)", text, re.M)))
    lines = ["resolved:"]
    for v in values:
        names = []
        for elf in elfs:
            r = subprocess.run([str(tools / "arm-none-eabi-addr2line.exe"), "-f", "-e", str(elf), v],
                               capture_output=True, text=True).stdout.split()
            if r and r[0] != "??":
                names.append(f"{r[0]} ({elf.stem})")
        lines.append(f"  {v}  {', '.join(names) or '?'}")
    # Stack words: the big-endian side stores them byte-swapped as gdb
    # sees them. Words that, swapped, point into the image's code are
    # likely return addresses: a rough backtrace per thread.
    lines.append("stack (big-endian words that point into the image's code):")
    image = elfs[0]
    syms = subprocess.run([str(tools / "arm-none-eabi-nm.exe"), str(image)], capture_output=True, text=True).stdout
    lo, hi = 0x00101000, 0
    for l in syms.splitlines():
        p = l.split()
        if len(p) == 3 and p[2] == "__be_text_end":
            hi = int(p[0], 16)
    thread = None
    for line in text.splitlines():
        m = re.match(r"^Thread (\d+) ", line)
        if m:
            thread = m.group(1)
            continue
        m = re.match(r"^0x[0-9a-f]+(?: <[^>]*>)?:\s+(.*)$", line)
        if not m or thread is None:
            continue
        for w in re.findall(r"0x([0-9a-f]{8})", m.group(1)):
            v = int.from_bytes(bytes.fromhex(w), "little")
            if lo <= v < hi:
                r = subprocess.run([str(tools / "arm-none-eabi-addr2line.exe"), "-f", "-e", str(image), hex(v)],
                                   capture_output=True, text=True).stdout.split()
                lines.append(f"  thread {thread}: {v:#x} {r[0] if r else '?'}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--azahar", default=r"C:\Program Files\Azahar\azahar.exe")
    ap.add_argument("--rom", type=Path, default=ROOT / "build" / "port" / "colosseum.3dsx")
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
            elf = args.rom.with_suffix(".elf").resolve()
            image = elf.parent / "image_final.elf"
            tools = Path(os.environ.get("DEVKITARM_WIN", r"C:\devkitPro\devkitARM")) / "bin"
            gdb = tools / "arm-none-eabi-gdb.exe"
            if image.exists():
                # Two images (tools/link_be.py): gdb reads the big-endian
                # stack as little-endian, so only pc and lr of each thread
                # are reliable; they are resolved against both images.
                cmds = ["set pagination off", "set confirm off", f"file {elf.as_posix()}",
                        "target remote localhost:24689", "break *0x0", "delete",
                        f"tbreak *{heartbeat_address(image, tools)}", "continue",
                        "info threads", "thread apply all info registers pc lr sp",
                        "thread apply all x/48xw $sp", "detach"]
            else:
                cmds = ["set pagination off", "set confirm off", f"file {elf.as_posix()}",
                        "target remote localhost:24689", "break __port_thread_dump", "continue",
                        "info threads", "thread apply all bt 12", "detach"]
            p = subprocess.run([str(gdb), "-batch", *sum((["-ex", c] for c in cmds), [])],
                               capture_output=True, text=True, timeout=args.seconds + 60)
            out = ROOT / "build" / "gdb.txt"
            text = p.stdout + p.stderr
            if image.exists():
                text += "\n" + resolve_registers(text, (image, elf), tools)
            out.write_text(text, encoding="utf-8")
            print(text[-7000:])
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
