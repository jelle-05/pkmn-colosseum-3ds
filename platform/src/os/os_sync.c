/*
 * os_sync.c - message queues, mutexes, condition variables and alarms.
 *
 * These follow the Dolphin SDK's own logic on top of the sleep queues and
 * the interrupt lock (os_thread.c, os_irq.c): each operation runs with
 * interrupts disabled, sleeps on the object's queue while it cannot proceed,
 * and wakes the other side. The objects keep their GameCube layouts, since
 * the game allocates them.
 */
#include <string.h>

#include "os_thread.h"

extern long long OSGetTime(void);

typedef void* OSMessage;

/* ---- message queues (0x20 bytes) ------------------------------------------ */

typedef struct {
    OSThreadQueue send;
    OSThreadQueue recv;
    OSMessage* msgs;
    s32 count;
    s32 first;
    s32 used;
} OSMessageQueue;

#define OS_MESSAGE_BLOCK 1

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgs, s32 count)
{
    OSInitThreadQueue(&mq->send);
    OSInitThreadQueue(&mq->recv);
    mq->msgs = msgs;
    mq->count = count;
    mq->first = 0;
    mq->used = 0;
}

int OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
    int level = OSDisableInterrupts();
    while (mq->count <= mq->used) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            OSRestoreInterrupts(level);
            return 0;
        }
        __port_sleep_on(&mq->send);
    }
    mq->msgs[(mq->first + mq->used) % mq->count] = msg;
    mq->used++;
    __port_wakeup(&mq->recv);
    OSRestoreInterrupts(level);
    return 1;
}

int OSJamMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
    int level = OSDisableInterrupts();
    while (mq->count <= mq->used) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            OSRestoreInterrupts(level);
            return 0;
        }
        __port_sleep_on(&mq->send);
    }
    mq->first = (mq->first + mq->count - 1) % mq->count;
    mq->msgs[mq->first] = msg;
    mq->used++;
    __port_wakeup(&mq->recv);
    OSRestoreInterrupts(level);
    return 1;
}

int OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags)
{
    int level = OSDisableInterrupts();
    while (mq->used == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            OSRestoreInterrupts(level);
            return 0;
        }
        __port_sleep_on(&mq->recv);
    }
    if (msg)
        *msg = mq->msgs[mq->first];
    mq->first = (mq->first + 1) % mq->count;
    mq->used--;
    __port_wakeup(&mq->send);
    OSRestoreInterrupts(level);
    return 1;
}

/* ---- mutexes (0x18 bytes) and condition variables (8 bytes) ------------- */

typedef struct {
    OSThreadQueue queue;
    void* thread;
    s32 count;
    void* link[2];
} OSMutex;

typedef struct {
    OSThreadQueue queue;
} OSCond;

void OSInitMutex(OSMutex* m)
{
    OSInitThreadQueue(&m->queue);
    m->thread = 0;
    m->count = 0;
}

void OSLockMutex(OSMutex* m)
{
    void* cur = OSGetCurrentThread();
    int level = OSDisableInterrupts();
    for (;;) {
        if (!m->thread) {
            m->thread = cur;
            m->count++;
            break;
        }
        if (m->thread == cur) {
            m->count++;
            break;
        }
        __port_sleep_on(&m->queue);
    }
    OSRestoreInterrupts(level);
}

int OSTryLockMutex(OSMutex* m)
{
    void* cur = OSGetCurrentThread();
    int ok = 0;
    int level = OSDisableInterrupts();
    if (!m->thread || m->thread == cur) {
        m->thread = cur;
        m->count++;
        ok = 1;
    }
    OSRestoreInterrupts(level);
    return ok;
}

void OSUnlockMutex(OSMutex* m)
{
    void* cur = OSGetCurrentThread();
    int level = OSDisableInterrupts();
    if (m->thread == cur && --m->count == 0) {
        m->thread = 0;
        __port_wakeup(&m->queue);
    }
    OSRestoreInterrupts(level);
}

void OSInitCond(OSCond* c) { OSInitThreadQueue(&c->queue); }

void OSWaitCond(OSCond* c, OSMutex* m)
{
    void* cur = OSGetCurrentThread();
    int level = OSDisableInterrupts();
    if (m->thread == cur) {
        s32 count = m->count;
        m->thread = 0;
        m->count = 0;
        __port_wakeup(&m->queue);
        __port_sleep_on(&c->queue);
        OSLockMutex(m);
        m->count = count;
    }
    OSRestoreInterrupts(level);
}

void OSSignalCond(OSCond* c) { __port_wakeup(&c->queue); }

/* ---- alarms (0x28 bytes) ------------------------------------------------------ */

typedef struct OSAlarm OSAlarm;
typedef void (*OSAlarmHandler)(OSAlarm* alarm, void* context);

struct OSAlarm {
    OSAlarmHandler handler;
    u32 tag;
    long long fire;
    OSAlarm* prev;
    OSAlarm* next;
    long long period;
    long long start;
};

void OSInitAlarm(void) {}

void OSCreateAlarm(OSAlarm* a)
{
    a->handler = 0;
    a->tag = 0;
}

static void alarm_fire(void* arg, long unused)
{
    (void)unused;
    OSAlarm* a = arg;
    OSAlarmHandler h = a->handler;
    if (!h)
        return;
    if (a->period) {
        a->fire += a->period;
        __port_irq_post(a->fire, alarm_fire, a, 0);
    } else {
        a->handler = 0;
    }
    h(a, 0);
}

void OSCancelAlarm(OSAlarm* a)
{
    int level = OSDisableInterrupts();
    __port_irq_cancel(alarm_fire, a);
    a->handler = 0;
    OSRestoreInterrupts(level);
}

void OSSetAlarm(OSAlarm* a, long long tick, OSAlarmHandler handler)
{
    int level = OSDisableInterrupts();
    __port_irq_cancel(alarm_fire, a);
    a->handler = handler;
    a->period = 0;
    a->fire = OSGetTime() + tick;
    __port_irq_post(a->fire, alarm_fire, a, 0);
    OSRestoreInterrupts(level);
}

void OSSetPeriodicAlarm(OSAlarm* a, long long start, long long period, OSAlarmHandler handler)
{
    int level = OSDisableInterrupts();
    __port_irq_cancel(alarm_fire, a);
    a->handler = handler;
    a->period = period;
    a->start = start;
    long long now = OSGetTime();
    a->fire = start;
    if (period > 0 && a->fire < now)
        a->fire += ((now - a->fire) / period + 1) * period;
    __port_irq_post(a->fire, alarm_fire, a, 0);
    OSRestoreInterrupts(level);
}

void OSSetAlarmTag(OSAlarm* a, u32 tag) { a->tag = tag; }
