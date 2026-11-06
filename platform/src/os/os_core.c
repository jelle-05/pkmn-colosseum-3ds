/*
 * os_core.c - the hardware-bound core of the Dolphin OS library.
 *
 * Portable SDK code (arena, heap allocator) is compiled from the decomp, see
 * platform/reuse.txt. This file covers what the GameCube did in hardware or
 * in OSInit before main(): the arena itself, interrupts, time, caches,
 * memory protection and debug output.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- arena ---------------------------------------------------------------
 * On the GameCube, OSInit sets the arena to the free RAM between the end of
 * the loaded program and the top of the 24 MB. For this game that is about
 * 19 MB; main() carves its 14.5 MB game heap and the OS heap out of it. */

#define GC_ARENA_SIZE 0x01300000u

void* __OSArenaLo;
void* __OSArenaHi;

__attribute__((constructor(101))) static void os_arena_init(void)
{
    void* arena = memalign(32, GC_ARENA_SIZE);
    if (!arena)
        svcBreak(USERBREAK_PANIC);
    __OSArenaLo = arena;
    __OSArenaHi = (char*)arena + GC_ARENA_SIZE;
}

/* ---- interrupts ------------------------------------------------------------
 * Game and SDK code brackets critical sections with OSDisableInterrupts /
 * OSRestoreInterrupts. There are no GameCube interrupts here; callbacks that
 * used to run from interrupts are driven by the platform layer. For now the
 * port runs the game on one thread, so a flag is enough. When threads come,
 * this becomes a recursive lock. */

static int interrupts_enabled = 1;

int __port_interrupts_enabled(void) { return interrupts_enabled; }

int OSDisableInterrupts(void)
{
    int old = interrupts_enabled;
    interrupts_enabled = 0;
    return old;
}

/* Work that arrived while interrupts were off is delivered when they come
 * back on, as a pending GameCube interrupt would fire then. */
extern void __port_dvd_pump(void);

int OSEnableInterrupts(void)
{
    int old = interrupts_enabled;
    interrupts_enabled = 1;
    if (!old)
        __port_dvd_pump();
    return old;
}

int OSRestoreInterrupts(int level)
{
    int old = interrupts_enabled;
    interrupts_enabled = level;
    if (level && !old)
        __port_dvd_pump();
    return old;
}

/* ---- time ------------------------------------------------------------------
 * GameCube time is counted at the timer clock, bus clock / 4 = 40.5 MHz, from
 * 2000-01-01 00:00 (the RTC sets the time base at boot). The game does
 * arithmetic on raw ticks (OSMillisecondsToTicks and friends) and converts
 * them to dates (OSTicksToCalendarTime, compiled from the decomp), so the
 * port keeps both the unit and the epoch: the 3DS clock gives the start,
 * the 3DS system tick the progress. OSGetTick is the low 32 bits of the same
 * counter, as on the GameCube. */

#define GC_TIMER_HZ 40500000ull
#define MS_1900_TO_2000 3155673600000ull   /* osGetTime() counts from 1900 */

static unsigned long long time_base;     /* GameCube ticks at system tick 0 */

static unsigned long long to_gc_ticks(unsigned long long sys)
{
    return (sys / SYSCLOCK_ARM11) * GC_TIMER_HZ + (sys % SYSCLOCK_ARM11) * GC_TIMER_HZ / SYSCLOCK_ARM11;
}

__attribute__((constructor(101))) static void os_time_init(void)
{
    unsigned long long ms = osGetTime();
    unsigned long long since_2000 = ms > MS_1900_TO_2000 ? ms - MS_1900_TO_2000 : 0;
    unsigned long long now = (since_2000 / 1000) * GC_TIMER_HZ + (since_2000 % 1000) * (GC_TIMER_HZ / 1000);
    time_base = now - to_gc_ticks(svcGetSystemTick());
}

long long OSGetTime(void)
{
    return (long long)(time_base + to_gc_ticks(svcGetSystemTick()));
}

unsigned long OSGetTick(void)
{
    return (unsigned long)OSGetTime();
}

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
        svcOutputDebugString(buf, n);
}

int DBIsDebuggerPresent(void) { return 0; }
