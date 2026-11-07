Testing on real hardware
========================

The port is tested on a New 3DS with Luma3DS, over ftpd. Everything goes
through the SD card; nothing needs the 3DS's screen except starting the
program.


Files on the SD card
--------------------

    /3ds/colosseum/GC6E01.iso       your own disc image (1,459,978,240 bytes)
    /3ds/colosseum/colosseum.3dsx   the port, started from the Homebrew Launcher
    /colosseum-port-script.txt      optional: buttons and screenshots by frame
    /colosseum-port.log             written by the port: log, heartbeat, profile
    /colosseum-port-frames.log      written by the port: presented frames
    /colosseum-shot-NNNNN.bmp       screenshots asked for by the script
    /luma/dumps/arm11/crash_dump_*.dmp   Luma's dump after a crash

The script has one event per line, counted in game frames:

    650 press START 8
    1020 press A 6
    1160 press UP 4
    1190 press A 6
    1450 shot

On hardware the game runs slower than in Azahar, so the same frame comes
later: at 5 frames a second, frame 1450 is five minutes in.


Copying files with ftpd
-----------------------

    curl -T build/port/colosseum.3dsx ftp://<3ds-ip>:5000/3ds/colosseum/colosseum.3dsx
    curl ftp://<3ds-ip>:5000/colosseum-port.log -o build/hw/colosseum-port.log

What we learned copying the 1.4 GB disc image (2026-11-07):

  - ftpd managed 0.7 MB/s at best, and as little as 0.03 MB/s on a weak
    signal. The connection dropped nine times; `curl -C - -T` resumes
    where it stopped. It took most of an afternoon. A card reader takes a
    few minutes.
  - Do not read part of a file over ftpd (`curl -r`): curl aborts the
    transfer when it has its bytes, and after an aborted transfer this
    ftpd refuses every new one ("451 Failed to listen on socket") until
    it is restarted. Whole-file downloads are fine.
  - To check a large file, let GodMode9 hash it on the console: select the
    file, "Calculate SHA-256". It writes <file>.sha next to it (32 bytes),
    which is small enough to fetch. GC6E01 (USA):
    78C4074429FA598B97BFAC296BF8E0F7E93F5CB6D8679F8D2653CA902820F6C5
  - 3dsDL cannot fetch from a PC on the local network: it only speaks
    HTTPS with certificates from the public CA list. tools/serve_disc.py
    (HTTP with Range requests) is there for downloaders that do speak
    plain HTTP.


Reading a Luma crash dump
-------------------------

The dump is a 40-byte header (magic, version, processor, exception type,
sizes), then r0-r12, sp, lr, pc, cpsr, dfsr, ifsr, far, fpexc, fpinst,
fpinst2, then code and stack. A pc in the host (little-endian, cpsr bit 9
clear) resolves with

    arm-none-eabi-addr2line -f -e build/port/colosseum.elf <pc> <lr>

Keep the colosseum.elf of the build that was on the console; addresses
change with every link.


Results so far
--------------

First run, 2026-11-07, New 3DS, build before 7b8dee4:

  - boots, loads, the title screen draws, START is taken, the memory card
    check runs; no unmapped memory accesses.
  - 5 frames a second on the title screen: 95 ms of vertex work a frame,
    against 18 ms in Azahar. 7b8dee4 turns on the New 3DS's 804 MHz clock
    and L2 cache (osSetSpeedupEnable, which was missing) and VFP RunFast
    mode, which should account for much of it. Not measured yet.
  - quitting crashed (data abort at 0x10 in hidScanInput, from the retrace
    thread after exit() had closed HID). Fixed in 7b8dee4, not tested on
    the console yet.
