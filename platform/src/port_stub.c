/*
 * port_stub.c - what an unimplemented SDK or game symbol does when called,
 * and the platform's other fatal stops.
 *
 * tools/link_probe.py generates one stub per missing function. Each stub
 * calls __port_stub() with its name, which prints the name to the debug
 * output (shown in Azahar's log and by Luma3DS's debugger) and stops.
 * The list of stubs is the work list for the platform layer.
 */
#include <string.h>

#include "port/host.h"

/* Last stub hit, for a debugger. */
const char* volatile __port_last_stub;

/* Writes `what` to the debug output and, as a line, to
 * sdmc:/colosseum-port-stub.log (Azahar filters debug output by default),
 * then stops. */
void __port_fatal(const char* what)
{
    static const char prefix[] = "port: ";
    char line[192];
    size_t n = strlen(what);
    if (n > sizeof(line) - sizeof(prefix) - 1)
        n = sizeof(line) - sizeof(prefix) - 1;
    memcpy(line, prefix, sizeof(prefix) - 1);
    memcpy(line + sizeof(prefix) - 1, what, n);
    host_debug_string(line, (host_u32)(sizeof(prefix) - 1 + n));

    int fd = host_file_open("sdmc:/colosseum-port-stub.log", "a");
    if (fd >= 0) {
        host_file_write(fd, what, (host_u32)n);
        host_file_write(fd, "\n", 1);
        host_file_close(fd);
    }
    host_break(0);
    for (;;) {
    }
}

void __port_stub(const char* name)
{
    __port_last_stub = name;
    __port_fatal(name);
}

/* Owned by the platform GX layer once it exists. */
unsigned char* __GXFifoPtr;
