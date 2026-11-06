/*
 * port_stub.c - what an unimplemented SDK or game symbol does when called.
 *
 * tools/link_probe.py generates one stub per missing function. Each stub
 * calls __port_stub() with its name, which prints the name to the debug
 * output (shown in Azahar's log and by Luma3DS's debugger) and stops.
 * The list of stubs is the work list for the platform layer.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* Last stub hit, for a debugger. */
const char* volatile __port_last_stub;

void __port_stub(const char* name)
{
    static const char prefix[] = "port: unimplemented ";
    char line[160];
    size_t n = strlen(name);

    __port_last_stub = name;
    if (n > sizeof(line) - sizeof(prefix))
        n = sizeof(line) - sizeof(prefix);
    memcpy(line, prefix, sizeof(prefix) - 1);
    memcpy(line + sizeof(prefix) - 1, name, n);
    svcOutputDebugString(line, (s32)(sizeof(prefix) - 1 + n));

    /* Also leave it on the SD card: Azahar filters debug output by default. */
    FILE* f = fopen("sdmc:/colosseum-port-stub.log", "a");
    if (f) {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
    svcBreak(USERBREAK_PANIC);
    for (;;) {
    }
}

/* Owned by the platform GX layer once it exists. */
unsigned char* __GXFifoPtr;
