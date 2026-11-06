/*
 * Port override of decomp include/dolphin/os/OSClock.h.
 *
 * The original binds __OSBusClock to low memory (0x800000F8) with MWCC's
 * `extern u32 x : addr;` syntax. The GameCube bus clock is a constant
 * 162 MHz, and the timer runs at a quarter of that. Game code keeps using
 * GameCube ticks; the platform's OSGetTick/OSGetTime convert from the 3DS
 * system tick, so tick arithmetic in game code keeps its original meaning.
 */
#ifndef DOLPHIN_OS_OSCLOCK_H
#define DOLPHIN_OS_OSCLOCK_H

#include "dolphin/types.h"

#define OS_BUS_CLOCK 162000000u
#define OS_TIMER_CLOCK (OS_BUS_CLOCK / 4)

/* Some decomp units read the variable directly. */
#define __OSBusClock OS_BUS_CLOCK

#define OSMillisecondsToTicks(msec) ((msec) * (OS_TIMER_CLOCK / 1000))
#define OSMicrosecondsToTicks(usec) (((usec) * (OS_TIMER_CLOCK / 125000)) / 8)

#endif /* DOLPHIN_OS_OSCLOCK_H */
