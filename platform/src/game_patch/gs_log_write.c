/*
 * gs_log_write.c - GSlogWrite, to the port log.
 *
 * The game logs errors and progress (missing files, bad IDs) with
 * GSlogWrite into a ring buffer in memory that only its debug menus show
 * (src/game/gs_log.cpp). Here every line goes to sdmc:/colosseum-port.log,
 * formatted by the game's own GSlogFormat, so the game's messages explain
 * what it is doing during bring-up.
 */
#include <stdarg.h>

extern void GSlogFormat(char* output, unsigned capacity, const char* format, va_list args);
extern void __port_log(const char* fmt, ...);

void GSlogWrite(const char* fmt, ...)
{
    char line[0x100];
    va_list ap;
    va_start(ap, fmt);
    GSlogFormat(line, 0xFF, fmt, ap);
    va_end(ap);
    line[0xFF] = 0;
    __port_log("game: %s", line);
}
