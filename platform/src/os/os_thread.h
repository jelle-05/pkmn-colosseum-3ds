/*
 * os_thread.h - internals shared by the platform's OS thread files.
 */
#ifndef PLATFORM_OS_THREAD_H
#define PLATFORM_OS_THREAD_H

#include <3ds.h>

/* An OSThreadQueue in the game's memory (8 bytes: head, tail). The platform
 * keeps its own wait list in it; game code only passes queues around. */
typedef struct PortThread PortThread;
typedef struct {
    PortThread* head;
    PortThread* tail;
} OSThreadQueue;

struct PortThread {
    void* os_thread;            /* the game's OSThread */
    Thread thread;              /* libctru thread, once started */
    Handle handle;              /* for priority changes */
    LightEvent wake;            /* one-shot: sleep, suspend, join */
    void* (*func)(void*);
    void* param;
    void* result;
    u32 stack_size;
    s32 suspend;
    s32 prio;                   /* GameCube priority, 0 (high) .. 31 */
    int started, terminated, detached;
    int irq_off;                /* holds the interrupt lock */
    OSThreadQueue* waiting_on;
    void* wait_from;            /* caller of the sleep, for the heartbeat log */
    PortThread* qnext;
    OSThreadQueue join_queue;
    int used;
};

int OSDisableInterrupts(void);
int OSEnableInterrupts(void);
int OSRestoreInterrupts(int level);
void* OSGetCurrentThread(void);
void OSInitThreadQueue(OSThreadQueue* q);

PortThread* __port_self(void);
void __port_sleep_on(OSThreadQueue* q);
void __port_wakeup(OSThreadQueue* q);
void __port_thread_dump(void);

/* Interrupt emulation (os_irq.c). */
extern RecursiveLock __port_irq_lock;
void __port_irq_post(long long when, void (*fn)(void*, long), void* arg, long value);
int __port_irq_cancel(void (*fn)(void*, long), void* arg);
void __port_irq_init(void);

/* GameCube priorities 0..31 map onto 3DS priorities 0x19..0x38; the
 * interrupt thread runs above them at 0x18 and the idle function below at
 * 0x3F. All of them share the application core, so exactly one runs at a
 * time and a higher-priority thread that becomes ready takes over, as on
 * the GameCube. */
#define PRIO_IRQ 0x18
#define PRIO_IDLE 0x3F
static inline int prio_3ds(int gc)
{
    if (gc < 0) gc = 0;
    if (gc > 31) gc = 31;
    return 0x19 + gc;
}

#endif /* PLATFORM_OS_THREAD_H */
