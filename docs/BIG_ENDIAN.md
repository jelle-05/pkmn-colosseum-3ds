Big-endian mode
===============

The GameCube is big-endian. Its data, the data compiled into the decomp
and every file on the disc (FSYS archives, HSD models, messages, scripts,
MusyX sound), is laid out that way. Converting all of it for a
little-endian CPU means knowing every structure, and the decomp does not
know most file formats yet.

The 3DS's ARM11 can do something else: SETEND BE switches its data
accesses to big-endian, while instructions stay little-endian (BE8). So
the port runs the game in that mode. GameCube data is then correct as it
is, compiled in or read from the disc, and so are bitfields (GCC lays
them out for big-endian ARM as MWCC does for PowerPC).

Tested on hardware (a 3DS with the Homebrew Launcher, 2026-11-07) and in
Azahar: big-endian reads give the GameCube values, and the mode survives
sleeps and thread switches (the kernel saves it with each thread).


Two images in one 3dsx
----------------------

libctru, citro3d and newlib are little-endian, and so is everything the
kernel and the GPU read. So the program is split:

    image (big-endian)   the game (build/unified), the platform layer
                         (platform/src), devkitARM's big-endian newlib for
                         strings and formatting, its big-endian libgcc
    host (little-endian) platform/host: libctru, citro3d, newlib, the
                         entry point

tools/link_be.py links the image as a BE8 program at fixed addresses and
the host as an ordinary 3dsx that carries it:

    3dsx code segment    crt0, the image's code and read-only data at
                         0x00101000, the host's code
    3dsx data segment    the image's data, then the host's
    3dsx bss             the image's zero-initialised data, then the host's

The image's data addresses depend on the size of the host's code, so the
link runs four passes (image for its sizes, host with placeholders, image
at the addresses found, host with the real image) and checks that nothing
moved.


Calls between the two
---------------------

The image calls the host only through the functions listed in
platform/include/port/host.h. platform/src/rt/host_stubs.S has a stub
for each: switch to little-endian, call through a table the host fills
at startup, switch back. The rules that keep this simple:

  - at most four arguments, all in registers: nothing on the stack (it
    would be read with the wrong byte order), no variadic functions, no
    structs by value;
  - no 64-bit integers (their word order in r0:r1 differs between the
    two modes); floats and doubles are fine, VFP registers have no byte
    order;
  - the host uses pointers into big-endian memory only as bytes (file
    reads, strings), or stores through be_store32;
  - libctru objects (locks, events, threads) are opaque storage the
    image never reads.

The host calls the image through host_be_call (platform/host/
host_switch.S): the entry point, and every thread the image creates.

The C math library comes from the host as well (HOST_LIBM in host.h):
devkitARM's big-endian newlib and libgcc are soft-float builds, whose
functions take doubles in core registers. link_be.py links them with
--no-warn-mismatch and refuses any of their floating-point functions the
image calls; rt/hf_conv.c has hard-float versions of the 64-bit
conversions. Variadic functions (sprintf) are fine in both ABIs.


What this replaced
------------------

The little-endian build needed the game's data converted at compile time:
tools/access_widths.py derived field layouts from the original PowerPC
code, tools/unify_prototypes.py byte-swapped initialisers, rebuilt
pointer tables and reversed bitfields (docs/DATA_ENDIANNESS.md). That
worked for compiled-in data but could never cover file data. With
compile_check.BIG_ENDIAN set those passes are off; the tools stay for
reference and for the data layout at GameCube offsets, which both builds
need (tools/gen_layout.py).

tools/link_probe.py is the little-endian link and no longer matches the
big-endian objects; link_be.py imports its symbol helpers.
