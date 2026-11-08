/*
 * prof.c - where the frame time goes, for the port log.
 *
 * A few places add the time they took to a slot; every 120 frames the
 * totals go to the log as milliseconds per frame. Times are GameCube
 * ticks (OSGetTime), 40.5 MHz.
 */
#include "port/prof.h"

extern long long OSGetTime(void);
extern void __port_log(const char* fmt, ...);

static long long total[PROF_SLOTS];
static unsigned count[PROF_SLOTS];
static long long window_start;
static unsigned frames;

static const char* const names[PROF_SLOTS] = {
    "vertices", "gpu submit", "frame end", "retrace wait", "irq lock wait", "dvd read",
    "irq handlers", " vtx decode", " vtx unlit", " vtx lit", "frame",
};

long long __prof_now(void) { return OSGetTime(); }

void __prof_add(int slot, long long ticks)
{
    total[slot] += ticks;
    count[slot]++;
}

/* Where the main thread blocks (other than the retrace wait), by caller. */
#define WAIT_SITES 8
static struct { void* from; long long ticks; unsigned n; } waits[WAIT_SITES];

void __prof_wait(void* from, long long ticks)
{
    int free_slot = -1;
    for (int i = 0; i < WAIT_SITES; i++) {
        if (waits[i].from == from) {
            waits[i].ticks += ticks;
            waits[i].n++;
            return;
        }
        if (!waits[i].from && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        waits[free_slot].from = from;
        waits[free_slot].ticks = ticks;
        waits[free_slot].n = 1;
    }
}

void __prof_frame(void)
{
    long long now = OSGetTime();
    if (!window_start)
        window_start = now;
    if (++frames < 120)
        return;
    long long span = now - window_start;
    __port_log("prof: %u frames in %d ms (%d.%d fps)", frames, (int)(span / 40500),
               (int)(frames * 40500000ll / span), (int)(frames * 405000000ll / span % 10));
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!count[i])
            continue;
        long long per = total[i] * 1000 / frames / 40500;        /* microseconds per frame */
        __port_log("  %-14s %5d.%d ms/frame (%u calls)", names[i], (int)(per / 1000), (int)(per % 1000 / 100),
                   count[i]);
        total[i] = 0;
        count[i] = 0;
    }
    for (int i = 0; i < WAIT_SITES; i++) {
        if (!waits[i].from)
            continue;
        long long per = waits[i].ticks * 1000 / frames / 40500;
        __port_log("  main blocked at %p: %d.%d ms/frame (%u times)", waits[i].from, (int)(per / 1000),
                   (int)(per % 1000 / 100), waits[i].n);
        waits[i].from = 0;
    }
    frames = 0;
    window_start = now;
}
