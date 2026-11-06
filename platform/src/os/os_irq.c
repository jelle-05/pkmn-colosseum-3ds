/*
 * os_irq.c - interrupts: the interrupt lock and the interrupt thread.
 *
 * On the GameCube, OSDisableInterrupts makes a section atomic: nothing
 * preempts the running thread until interrupts come back on, and work that
 * arrived meanwhile (retrace, alarm, DVD completion) runs right then, in
 * interrupt context. Game code relies on this everywhere, including busy
 * loops that wait for an interrupt to set a flag.
 *
 * Here interrupts disabled means holding __port_irq_lock, a recursive lock.
 * Interrupt work is posted to the interrupt thread, the highest-priority
 * thread on the application core, which takes the lock before running any
 * of it. While a game thread has interrupts disabled, the interrupt thread
 * waits; when they come back on, it takes over at once and delivers.
 *
 * Work is posted with a due time in GameCube ticks (OSGetTime), which also
 * makes this the timer behind OSAlarm and the latency of DVD reads.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "os_thread.h"

extern long long OSGetTime(void);
extern void __port_log(const char* fmt, ...);

RecursiveLock __port_irq_lock;

typedef struct Item {
    long long when;
    void (*fn)(void*, long);
    void* arg;
    long value;
    struct Item* next;
} Item;

#define POOL 512
static Item pool[POOL];
static Item* free_list;
static Item* pending;            /* sorted by due time */
static LightLock pending_lock;
static LightEvent irq_event;
static PortThread irq_self;
static int initialised;

static long long ticks_to_ns(long long t) { return t * 1000000000ll / 40500000ll; }

void __port_irq_post(long long when, void (*fn)(void*, long), void* arg, long value)
{
    LightLock_Lock(&pending_lock);
    Item* it = free_list;
    if (!it) {
        LightLock_Unlock(&pending_lock);
        __port_log("irq: too many pending interrupts");
        svcBreak(USERBREAK_PANIC);
    }
    free_list = it->next;
    it->when = when;
    it->fn = fn;
    it->arg = arg;
    it->value = value;
    Item** p = &pending;
    while (*p && (*p)->when <= when)
        p = &(*p)->next;
    it->next = *p;
    *p = it;
    LightLock_Unlock(&pending_lock);
    LightEvent_Signal(&irq_event);
}

int __port_irq_cancel(void (*fn)(void*, long), void* arg)
{
    int n = 0;
    LightLock_Lock(&pending_lock);
    for (Item** p = &pending; *p;) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            Item* it = *p;
            *p = it->next;
            it->next = free_list;
            free_list = it;
            n++;
        } else {
            p = &(*p)->next;
        }
    }
    LightLock_Unlock(&pending_lock);
    return n;
}

static Item* pop_due(long long now)
{
    LightLock_Lock(&pending_lock);
    Item* it = pending;
    if (it && it->when <= now)
        pending = it->next;
    else
        it = 0;
    LightLock_Unlock(&pending_lock);
    return it;
}

static void irq_thread(void* unused)
{
    (void)unused;
    extern void __port_set_self(PortThread*);
    __port_set_self(&irq_self);
    for (;;) {
        LightLock_Lock(&pending_lock);
        long long next = pending ? pending->when : -1;
        LightLock_Unlock(&pending_lock);
        long long now = OSGetTime();
        if (next < 0)
            LightEvent_Wait(&irq_event);
        else if (next > now)
            LightEvent_WaitTimeout(&irq_event, ticks_to_ns(next - now));

        now = OSGetTime();
        Item* it = pop_due(now);
        if (!it)
            continue;
        /* Interrupt context: interrupts are off while handlers run. */
        RecursiveLock_Lock(&__port_irq_lock);
        irq_self.irq_off = 1;
        while (it) {
            void (*fn)(void*, long) = it->fn;
            void* arg = it->arg;
            long value = it->value;
            LightLock_Lock(&pending_lock);
            it->next = free_list;
            free_list = it;
            LightLock_Unlock(&pending_lock);
            fn(arg, value);
            it = pop_due(OSGetTime());
        }
        irq_self.irq_off = 0;
        RecursiveLock_Unlock(&__port_irq_lock);
    }
}

void __port_irq_init(void)
{
    if (initialised)
        return;
    initialised = 1;
    RecursiveLock_Init(&__port_irq_lock);
    LightLock_Init(&pending_lock);
    LightEvent_Init(&irq_event, RESET_ONESHOT);
    for (int i = 0; i < POOL - 1; i++)
        pool[i].next = &pool[i + 1];
    free_list = &pool[0];
    memset(&irq_self, 0, sizeof(irq_self));
    irq_self.prio = -1;
    if (!threadCreate(irq_thread, 0, 32 * 1024, PRIO_IRQ, -2, true)) {
        __port_log("irq: cannot create the interrupt thread");
        svcBreak(USERBREAK_PANIC);
    }
}

/* ---- the interrupt API ----------------------------------------------------
 * Per thread, as on the GameCube, where the enable bit is saved with each
 * thread's context. */

int OSDisableInterrupts(void)
{
    PortThread* t = __port_self();
    if (t->irq_off)
        return 0;
    RecursiveLock_Lock(&__port_irq_lock);
    t->irq_off = 1;
    return 1;
}

int OSEnableInterrupts(void)
{
    PortThread* t = __port_self();
    if (!t->irq_off)
        return 1;
    t->irq_off = 0;
    RecursiveLock_Unlock(&__port_irq_lock);
    return 0;
}

int OSRestoreInterrupts(int level)
{
    PortThread* t = __port_self();
    int was = !t->irq_off;
    if (level && !was) {
        t->irq_off = 0;
        RecursiveLock_Unlock(&__port_irq_lock);
    } else if (!level && was) {
        RecursiveLock_Lock(&__port_irq_lock);
        t->irq_off = 1;
    }
    return was;
}
