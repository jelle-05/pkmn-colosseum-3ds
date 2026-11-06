/*
 * os_core.c - the hardware-bound core of the Dolphin OS library.
 *
 * Portable SDK code (arena, heap allocator) is compiled from the decomp, see
 * platform/reuse.txt. This file covers what the GameCube did in hardware or
 * in OSInit before main(): the arena itself, interrupts, time, caches,
 * memory protection and debug output.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "port/host.h"

/* ---- arena ---------------------------------------------------------------
 * On the GameCube, OSInit sets the arena to the free RAM between the end of
 * the loaded program and the top of the 24 MB. For this game that is about
 * 19 MB; main() carves its 14.5 MB game heap and the OS heap out of it. */

#define GC_ARENA_SIZE 0x01300000u

/* __OSArenaHi is the game's own .sbss variable (src/game/data/
 * sbss_8047A6E8.c); __OSArenaLo lived in the SDK's OS data. */
void* __OSArenaLo;
extern void* __OSArenaHi;

/* Appends a line to sdmc:/colosseum-port.log; for bring-up diagnostics. */
void __port_log(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (n < sizeof(buf) - 1)
        buf[n++] = '\n';
    int fd = host_file_open("sdmc:/colosseum-port.log", "a");
    if (fd >= 0) {
        host_file_write(fd, buf, n);
        host_file_close(fd);
    }
}

__attribute__((constructor(101))) static void os_arena_init(void)
{
    void* arena = host_memalign(32, GC_ARENA_SIZE);
    if (!arena)
        host_break(0);
    __OSArenaLo = arena;
    __OSArenaHi = (char*)arena + GC_ARENA_SIZE;
    __port_log("arena %p-%p", __OSArenaLo, __OSArenaHi);
}

/* Interrupts are in os_irq.c, threads in os_thread.c. */

/* ---- time ------------------------------------------------------------------
 * GameCube time is counted at the timer clock, bus clock / 4 = 40.5 MHz, from
 * 2000-01-01 00:00 (the RTC sets the time base at boot). The game does
 * arithmetic on raw ticks (OSMillisecondsToTicks and friends) and converts
 * them to dates (OSTicksToCalendarTime, compiled from the decomp), so the
 * port keeps both the unit and the epoch: the 3DS clock gives the start,
 * the 3DS system tick the progress. OSGetTick is the low 32 bits of the same
 * counter, as on the GameCube. */

long long OSGetTime(void)
{
    unsigned int t[2];
    host_gc_time(t);
    return (long long)((unsigned long long)t[0] << 32 | t[1]);
}

unsigned long OSGetTick(void)
{
    return (unsigned long)OSGetTime();
}

/* ---- memory size -------------------------------------------------------------
 * The SDK reads these from low memory (0x80000028 and 0x800000F0), which
 * gc_lowmem.c fills with the retail values: 24 MB. */

#include "port/gc_mem.h"

unsigned long OSGetPhysicalMemSize(void) { return *(unsigned long*)__GC_ADDR(0x80000028); }
unsigned long OSGetConsoleSimulatedMemSize(void) { return *(unsigned long*)__GC_ADDR(0x800000F0); }

/* ---- caches and memory protection -----------------------------------------
 * The GameCube needs explicit flushes before DMA and the GPU read memory.
 * Our DMA is memcpy and our GPU layer reads from the CPU's view, so these
 * have nothing to do. The GPU flush the 3DS does need lives in platform/gx. */

void DCFlushRange(void* addr, unsigned long n) { (void)addr; (void)n; }
void DCFlushRangeNoSync(void* addr, unsigned long n) { (void)addr; (void)n; }
void DCStoreRange(void* addr, unsigned long n) { (void)addr; (void)n; }
void DCStoreRangeNoSync(void* addr, unsigned long n) { (void)addr; (void)n; }
void DCInvalidateRange(void* addr, unsigned long n) { (void)addr; (void)n; }
void ICInvalidateRange(void* addr, unsigned long n) { (void)addr; (void)n; }
void PPCSync(void) {}

/* Debug-only write protection of memory ranges on the GameCube. */
void OSProtectRange(unsigned long chan, void* addr, unsigned long n, unsigned long control)
{
    (void)chan; (void)addr; (void)n; (void)control;
}

/* ---- error handlers ----------------------------------------------------------
 * The game installs a crash screen for PowerPC exceptions (DSI, alignment,
 * ...). Those exceptions do not exist here. The handlers are kept so that a
 * later 3DS exception hook can call the game's own crash report. */

typedef void (*OSErrorHandler)(unsigned short error, void* context, ...);

#define OS_ERROR_MAX 17
static OSErrorHandler error_handlers[OS_ERROR_MAX];

OSErrorHandler OSSetErrorHandler(unsigned short error, OSErrorHandler handler)
{
    if (error >= OS_ERROR_MAX)
        return 0;
    OSErrorHandler old = error_handlers[error];
    error_handlers[error] = handler;
    return old;
}

/* ---- debug output ------------------------------------------------------- */

void OSReport(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    if (n > 0)
        host_debug_string(buf, (unsigned)n);
}

int DBIsDebuggerPresent(void) { return 0; }

/* ---- video and sound settings kept in SRAM on the GameCube --------------- */

static unsigned long progressive_mode, sound_mode = 1;   /* OS_SOUND_MODE_STEREO */

unsigned long OSGetProgressiveMode(void) { return progressive_mode; }
void OSSetProgressiveMode(unsigned long on) { progressive_mode = on; }
unsigned long OSGetSoundMode(void) { return sound_mode; }
void OSSetSoundMode(unsigned long mode) { sound_mode = mode; }

/* ---- reset and exceptions -------------------------------------------------
 * The reset button and hot reset have no 3DS meaning. A reset request ends
 * the program; the HOME menu is how a 3DS app is left. */

void OSRegisterResetFunction(void* info) { (void)info; }
unsigned long OSGetResetCode(void) { return 0; }
unsigned long OSGetResetButtonState(void) { return 0; }
void OSResetSystem(int reset, unsigned long code, int force) { (void)reset; (void)code; (void)force; host_exit(0); }
void __OSDoHotReset(unsigned long code) { (void)code; host_exit(0); }
void* __OSSetExceptionHandler(unsigned char exception, void* handler) { (void)exception; (void)handler; return 0; }
