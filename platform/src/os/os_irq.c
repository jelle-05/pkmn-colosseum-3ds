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
#include <string.h>

#include "os_thread.h"

extern long long OSGetTime(void);
extern void __port_log(const char* fmt, ...);

host_rlock __port_irq_lock;

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
        __port_irq_take(&irq_self, (void*)irq_thread);
        while (it) {
            void (*fn)(void*, long) = it->fn;
            void* arg = it->arg;
            long value = it->value;
            host_lock_lock(&pending_lock);
            it->next = free_list;
            free_list = it;
            host_lock_unlock(&pending_lock);
            fn(arg, value);
            it = pop_due(OSGetTime());
        }
        __port_irq_drop(&irq_self);
    }
}

void __port_irq_init(void)
{
    if (initialised)
        return;
    initialised = 1;
    host_rlock_init(&__port_irq_lock);
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

/* ---- taking the lock -------------------------------------------------------
 * On the GameCube a thread with interrupts off cannot be preempted. Here the
 * lock holder can be, by a higher-priority thread that does not need the
 * lock, while a thread that does need it waits: a low-priority holder (the
 * idle function takes it too) would then never get to release it. So a
 * thread that finds the lock taken raises the holder above every game
 * thread, to the interrupt thread's priority, until it lets go. */

void __port_irq_take(PortThread* t, void* where)
{
    while (!host_rlock_trylock(&__port_irq_lock)) {
        /* The holder records itself just after taking the lock; until it
         * has, it is unknown, and blocking now could wait for a thread
         * that never gets the CPU. So look again shortly. */
        PortThread* o = __port_irq_owner;
        if (!o || o == t) {
            host_sleep_us(20);
            continue;
        }
        if (o->handle && !o->boosted) {
            o->boosted = 1;
            host_thread_set_priority(o->handle, PRIO_IRQ);
        }
        host_rlock_lock(&__port_irq_lock);
        break;
    }
    t->irq_off = 1;
    __port_irq_owner = t;
    __port_irq_taken_at = where;
}

void __port_irq_drop(PortThread* t)
{
    int boosted = t->boosted;
    t->boosted = 0;
    t->irq_off = 0;
    __port_irq_owner = 0;
    host_rlock_unlock(&__port_irq_lock);
    if (boosted)
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