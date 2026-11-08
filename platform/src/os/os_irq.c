/*
 * os_irq.c - interrupts: the interrupt lock and the interrupt thread.
 *
 * On the GameCube, OSDisableInterrupts makes a section atomic: nothing
 * preempts the running thread until interrupts come back on, and work that
 * arrived meanwhile (retrace, alarm, DVD completion) runs right then, in
 * interrupt context. Game code relies on this everywhere, including busy
 * loops that wait for an interrupt to set a flag.
 *
 * Here interrupts disabled means holding the interrupt lock (below).
 * Interrupt work is posted to the interrupt thread, the highest-priority
 * thread on the application core, which takes the lock before running any
 * of it. While a game thread has interrupts disabled, the interrupt thread
 * waits; when they come back on, it takes over at once and delivers.
 *
 * Work is posted with a due time in GameCube ticks (OSGetTime), which also
 * makes this the timer behind OSAlarm and the latency of DVD reads.
 */
#include <string.h>

#include "os_thread.h"
#include "port/prof.h"

extern long long OSGetTime(void);
extern void __port_log(const char* fmt, ...);


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
static host_lock pending_lock;
static host_event irq_event;
static PortThread irq_self;
static int initialised;

/* The interrupt lock (see below). */
static host_event lock_free;          /* signalled when the lock is let go */
static volatile int lock_waiters;

/* What the interrupt thread is doing, for the heartbeat. */
volatile int __port_irq_state;          /* 0 idle, 1 waiting for the lock, 2 in a handler */
void* volatile __port_irq_handler;
volatile unsigned __port_irq_posted, __port_irq_run;

/* Who holds the interrupt lock and where it was taken, for the heartbeat. */
PortThread* volatile __port_irq_owner;
void* volatile __port_irq_taken_at;

static u32 ticks_to_us(long long t)
{
    long long us = t * 1000000ll / 40500000ll;
    return us > 0xFFFFFFF0ll ? 0xFFFFFFF0u : (u32)us;
}

void __port_irq_post(long long when, void (*fn)(void*, long), void* arg, long value)
{
    host_lock_lock(&pending_lock);
    Item* it = free_list;
    if (!it) {
        host_lock_unlock(&pending_lock);
        __port_log("irq: too many pending interrupts");
        host_break(0);
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
    __port_irq_posted++;
    host_lock_unlock(&pending_lock);
    host_event_signal(&irq_event);
}

int __port_irq_cancel(void (*fn)(void*, long), void* arg)
{
    int n = 0;
    host_lock_lock(&pending_lock);
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
    host_lock_unlock(&pending_lock);
    return n;
}

static Item* pop_due(long long now)
{
    host_lock_lock(&pending_lock);
    Item* it = pending;
    if (it && it->when <= now)
        pending = it->next;
    else
        it = 0;
    host_lock_unlock(&pending_lock);
    return it;
}

static void irq_thread(void* unused)
{
    (void)unused;
    extern void __port_set_self(PortThread*);
    __port_set_self(&irq_self);
    for (;;) {
        host_lock_lock(&pending_lock);
        long long next = pending ? pending->when : -1;
        host_lock_unlock(&pending_lock);
        long long now = OSGetTime();
        if (next < 0)
            host_event_wait(&irq_event);
        else if (next > now)
            host_event_wait_us(&irq_event, ticks_to_us(next - now));

        now = OSGetTime();
        Item* it = pop_due(now);
        if (!it)
            continue;
        /* Interrupt context: interrupts are off while handlers run. */
        __port_irq_state = 1;
        __port_irq_take(&irq_self, (void*)irq_thread);
        __port_irq_state = 2;
        while (it) {
            void (*fn)(void*, long) = it->fn;
            void* arg = it->arg;
            long value = it->value;
            host_lock_lock(&pending_lock);
            it->next = free_list;
            free_list = it;
            host_lock_unlock(&pending_lock);
            __port_irq_handler = (void*)fn;
            __port_irq_run++;
            PROF_BEGIN(t_irq);
            fn(arg, value);
            PROF_END(PROF_IRQ_HANDLERS, t_irq);
            it = pop_due(OSGetTime());
        }
        __port_irq_drop(&irq_self);
        __port_irq_state = 0;
    }
}

void __port_irq_init(void)
{
    if (initialised)
        return;
    initialised = 1;
    host_event_init(&lock_free, 1);
    host_lock_init(&pending_lock);
    host_event_init(&irq_event, 1);
    for (int i = 0; i < POOL - 1; i++)
        pool[i].next = &pool[i + 1];
    free_list = &pool[0];
    memset(&irq_self, 0, sizeof(irq_self));
    irq_self.prio = -1;
    irq_self.prio3ds = PRIO_IRQ;
    if (!host_thread_create(irq_thread, 0, 32 * 1024, PRIO_IRQ | 1 << 8)) {
        __port_log("irq: cannot create the interrupt thread");
        host_break(0);
    }
}

/* ---- the lock ----------------------------------------------------------------
 * On the GameCube a thread with interrupts off cannot be preempted, and game
 * code relies on it: the idle function disables interrupts at the lowest
 * priority, while the main thread spins in OSYieldThread waiting for the
 * retrace interrupt. Here the holder could be preempted by a thread that
 * does not need the lock, while the interrupt thread waits for it. So the
 * holder raises itself to the interrupt thread's priority for as long as it
 * holds the lock: no game thread preempts it, as on the GameCube.
 *
 * The lock is the owner word, taken by compare-and-swap. */

volatile unsigned __port_irq_takes;   /* for the heartbeat */
volatile unsigned __port_irq_takes_main, __port_irq_takes_raised;

void __port_irq_take(PortThread* t, void* where)
{
    if (!initialised)
        __port_irq_init();
    int raise = t->handle && t->prio3ds != PRIO_IRQ;
    if (raise)
        host_thread_set_priority(t->handle, PRIO_IRQ);
    long long t_wait = 0;
    while (!__sync_bool_compare_and_swap(&__port_irq_owner, (PortThread*)0, t)) {
        if (!t_wait)
            t_wait = __prof_now();
        __sync_fetch_and_add(&lock_waiters, 1);
        if (__port_irq_owner)
            host_event_wait_us(&lock_free, 1000);   /* the timeout covers a lost signal */
        __sync_fetch_and_sub(&lock_waiters, 1);
    }
    if (t_wait)
        PROF_END(PROF_IRQ_WAIT, t_wait);
    t->boosted = raise;
    t->irq_off = 1;
    __port_irq_taken_at = where;
    __port_irq_takes++;
    if (t->thread == 0)
        __port_irq_takes_main++;
    if (raise)
        __port_irq_takes_raised++;
}

void __port_irq_drop(PortThread* t)
{
    int raised = t->boosted;
    t->boosted = 0;
    t->irq_off = 0;
    __sync_synchronize();
    __port_irq_owner = 0;
    if (lock_waiters)
        host_event_signal(&lock_free);
    if (raised)
        host_thread_set_priority(t->handle, t->prio3ds);
}
/* ---- the interrupt API ----------------------------------------------------
 * Per thread, as on the GameCube, where the enable bit is saved with each
 * thread's context. */

int OSDisableInterrupts(void)
{
    PortThread* t = __port_self();
    if (t->irq_off)
        return 0;
    __port_irq_take(t, __builtin_return_address(0));
    return 1;
}

int OSEnableInterrupts(void)
{
    PortThread* t = __port_self();
    if (!t->irq_off)
        return 1;
    __port_irq_drop(t);
    return 0;
}

int OSRestoreInterrupts(int level)
{
    PortThread* t = __port_self();
    int was = !t->irq_off;
    if (level && !was)
        __port_irq_drop(t);
    else if (!level && was)
        __port_irq_take(t, __builtin_return_address(0));
    return was;
}