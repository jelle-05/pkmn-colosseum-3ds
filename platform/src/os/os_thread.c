/*
 * os_thread.c - Dolphin OS threads on 3DS threads.
 *
 * Every OSThread becomes a 3DS thread on the application core, at a priority
 * mapped from its GameCube priority (os_thread.h). With one core and strict
 * priority scheduling, the 3DS kernel gives the guarantees the Dolphin OS
 * gave: one thread runs at a time, and the highest-priority ready thread
 * runs. Blocking (sleep queues, suspension, joins) uses a one-shot event per
 * thread; the queue bookkeeping is guarded by a short internal lock, never
 * held while blocked.
 *
 * Game code brackets sleeps with OSDisableInterrupts, as the SDK requires.
 * A thread that blocks while holding the interrupt lock gives it up for the
 * wait and takes it back after, as the GameCube's OSSleepThread enables
 * interrupts while the thread is off the CPU.
 *
 * The idle function (OSSetIdleFunction) runs on a thread of its own below
 * all others, so it runs exactly when every game thread is waiting. The game
 * uses it for background tasks in a loop that never yields.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "os_thread.h"

extern void __port_log(const char* fmt, ...);

#define MAX_THREADS 32
#define OSTHREAD_SIZE 0x318

static PortThread threads[MAX_THREADS];
static PortThread main_thread, idle_thread;
static unsigned char default_os_thread[OSTHREAD_SIZE] __attribute__((aligned(8)));
static LightLock sched_lock;
static __thread PortThread* self_tls;
static s32 scheduler_disabled;

void __port_set_self(PortThread* t) { self_tls = t; }
PortThread* __port_self(void) { return self_tls ? self_tls : &main_thread; }

/* The SDK's OSThread fields a debugger or game code might look at. */
enum { OS_THREAD_STATE_READY = 1, OS_THREAD_STATE_RUNNING = 2, OS_THREAD_STATE_WAITING = 4,
       OS_THREAD_STATE_MORIBUND = 8 };
static void set_state(PortThread* t, unsigned short state)
{
    if (t->os_thread)
        *(unsigned short*)((char*)t->os_thread + 0x2C8) = state;
}

__attribute__((constructor(102))) static void os_thread_init(void)
{
    LightLock_Init(&sched_lock);
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.os_thread = default_os_thread;
    main_thread.prio = 16;                       /* the SDK's default thread */
    main_thread.started = 1;
    main_thread.used = 1;
    LightEvent_Init(&main_thread.wake, RESET_ONESHOT);
    svcDuplicateHandle(&main_thread.handle, CUR_THREAD_HANDLE);
    svcSetThreadPriority(CUR_THREAD_HANDLE, prio_3ds(main_thread.prio));
    self_tls = &main_thread;
    __port_irq_init();
}

static PortThread* find(void* os_thread)
{
    if (!os_thread || os_thread == default_os_thread)
        return &main_thread;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].os_thread == os_thread)
            return &threads[i];
    return 0;
}

/* ---- wait queues ------------------------------------------------------------ */

void OSInitThreadQueue(OSThreadQueue* q) { q->head = q->tail = 0; }

static void block(PortThread* t)
{
    int had = t->irq_off;
    if (had) {
        t->irq_off = 0;
        RecursiveLock_Unlock(&__port_irq_lock);
    }
    set_state(t, OS_THREAD_STATE_WAITING);
    LightEvent_Wait(&t->wake);
    set_state(t, OS_THREAD_STATE_RUNNING);
    if (had) {
        RecursiveLock_Lock(&__port_irq_lock);
        t->irq_off = 1;
    }
}

static void sleep_on(OSThreadQueue* q, void* from)
{
    PortThread* t = __port_self();
    LightLock_Lock(&sched_lock);
    t->waiting_on = q;
    t->wait_from = from;
    t->qnext = 0;
    if (q->tail)
        q->tail->qnext = t;
    else
        q->head = t;
    q->tail = t;
    LightLock_Unlock(&sched_lock);
    block(t);
    t->wait_from = 0;
}

void __port_sleep_on(OSThreadQueue* q) { sleep_on(q, __builtin_return_address(0)); }

/* Heartbeat for the log: what every thread is doing. Called from the vblank
   thread; reads without locking, good enough for a diagnosis. */
void __port_thread_dump(void)
{
    PortThread* list[MAX_THREADS + 1];
    int n = 0;
    list[n++] = &main_thread;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used)
            list[n++] = &threads[i];
    for (int i = 0; i < n; i++) {
        PortThread* t = list[i];
        __port_log("  thread %p prio %d %s%s%s queue %p from %p", t->os_thread, (int)t->prio,
                   t->terminated ? "terminated " : !t->started && t != &main_thread ? "not started " : "",
                   t->suspend > 0 ? "suspended " : "", t->irq_off ? "irq-off " : "",
                   (void*)t->waiting_on, t->wait_from);
    }
}

void __port_wakeup(OSThreadQueue* q)
{
    LightLock_Lock(&sched_lock);
    PortThread* t = q->head;
    q->head = q->tail = 0;
    while (t) {
        PortThread* next = t->qnext;
        t->qnext = 0;
        t->waiting_on = 0;
        LightEvent_Signal(&t->wake);
        t = next;
    }
    LightLock_Unlock(&sched_lock);
}

void OSSleepThread(OSThreadQueue* q) { sleep_on(q, __builtin_return_address(0)); }
void OSWakeupThread(OSThreadQueue* q) { __port_wakeup(q); }

/* ---- thread lifetime ------------------------------------------------------------ */

void OSExitThread(void* result);

static void entry(void* arg)
{
    PortThread* t = arg;
    self_tls = t;
    set_state(t, OS_THREAD_STATE_RUNNING);
    void* result = t->func(t->param);
    OSExitThread(result);
}

int OSCreateThread(void* os_thread, void* (*func)(void*), void* param, void* stack, u32 stack_size,
                   s32 prio, u16 attr)
{
    (void)stack;
    if (prio < 0 || prio > 31)
        return 0;
    PortThread* t = find(os_thread);
    if (!t || t == &main_thread) {
        for (int i = 0; i < MAX_THREADS && !t; i++)
            if (!threads[i].used || threads[i].terminated)
                t = &threads[i];
    }
    if (!t) {
        __port_log("OSCreateThread: more than %d threads", MAX_THREADS);
        return 0;
    }
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->os_thread = os_thread;
    t->func = func;
    t->param = param;
    t->stack_size = stack_size;
    t->prio = prio;
    t->suspend = 1;                     /* created suspended, as the SDK does */
    t->detached = (attr & 1) != 0;      /* OS_THREAD_ATTR_DETACH */
    LightEvent_Init(&t->wake, RESET_ONESHOT);
    set_state(t, OS_THREAD_STATE_READY);
    return 1;
}

s32 OSResumeThread(void* os_thread)
{
    PortThread* t = find(os_thread);
    if (!t)
        return 0;
    LightLock_Lock(&sched_lock);
    s32 prev = t->suspend;
    if (t->suspend > 0)
        t->suspend--;
    int go = t->suspend == 0 && prev > 0;
    LightLock_Unlock(&sched_lock);
    if (go) {
        if (!t->started) {
            t->started = 1;
            u32 size = t->stack_size < 64 * 1024 ? 64 * 1024 : t->stack_size * 2;
            t->thread = threadCreate(entry, t, size, prio_3ds(t->prio), -2, false);
            if (!t->thread) {
                __port_log("OSResumeThread: cannot create a thread");
                svcBreak(USERBREAK_PANIC);
            }
            t->handle = threadGetHandle(t->thread);
        } else {
            LightEvent_Signal(&t->wake);
        }
    }
    return prev;
}

/* Suspending another thread only takes effect when it next blocks; the
 * game only suspends itself (to sleep until an alarm resumes it). */
s32 OSSuspendThread(void* os_thread)
{
    PortThread* t = find(os_thread);
    if (!t)
        return 0;
    LightLock_Lock(&sched_lock);
    s32 prev = t->suspend++;
    LightLock_Unlock(&sched_lock);
    if (t == __port_self())
        while (t->suspend > 0)
            block(t);
    return prev;
}

void OSExitThread(void* result)
{
    PortThread* t = __port_self();
    t->result = result;
    t->terminated = 1;
    set_state(t, OS_THREAD_STATE_MORIBUND);
    __port_wakeup(&t->join_queue);
    if (t->thread)
        threadExit(0);
    /* the main thread exiting ends the program */
    for (;;)
        svcSleepThread(1000000000ll);
}

void OSCancelThread(void* os_thread)
{
    PortThread* t = find(os_thread);
    if (!t)
        return;
    if (t == __port_self())
        OSExitThread(0);
    t->terminated = 1;          /* another thread: cannot be stopped from here */
    __port_wakeup(&t->join_queue);
}

int OSJoinThread(void* os_thread, void** result)
{
    PortThread* t = find(os_thread);
    if (!t || t->detached)
        return 0;
    int level = OSDisableInterrupts();
    while (!t->terminated)
        __port_sleep_on(&t->join_queue);
    OSRestoreInterrupts(level);
    if (result)
        *result = t->result;
    return 1;
}

void OSDetachThread(void* os_thread)
{
    PortThread* t = find(os_thread);
    if (t)
        t->detached = 1;
}

int OSIsThreadTerminated(void* os_thread)
{
    PortThread* t = find(os_thread);
    return !t || t->terminated;
}

int OSIsThreadSuspended(void* os_thread)
{
    PortThread* t = find(os_thread);
    return t && t->suspend > 0;
}

void* OSGetCurrentThread(void) { return __port_self()->os_thread; }

int OSSetThreadPriority(void* os_thread, s32 prio)
{
    PortThread* t = find(os_thread);
    if (!t || prio < 0 || prio > 31)
        return 0;
    t->prio = prio;
    if (t->handle)
        svcSetThreadPriority(t->handle, prio_3ds(prio));
    return 1;
}

s32 OSGetThreadPriority(void* os_thread)
{
    PortThread* t = find(os_thread);
    return t ? t->prio : 0;
}

void OSYieldThread(void) { svcSleepThread(0); }

/* The kernel schedules the threads; these only keep the SDK's count. */
s32 OSDisableScheduler(void) { return scheduler_disabled++; }
s32 OSEnableScheduler(void) { return scheduler_disabled--; }
void __OSReschedule(void) {}

/* ---- the idle function ------------------------------------------------------ */

static void (*idle_func)(void*);
static void* idle_param;
static int idle_running;

static void idle_entry(void* unused)
{
    (void)unused;
    self_tls = &idle_thread;
    for (;;) {
        if (idle_func)
            idle_func(idle_param);
        svcSleepThread(1000000);      /* only if the function ever returns */
    }
}

void* OSSetIdleFunction(void (*func)(void*), void* param, void* stack, u32 stack_size)
{
    (void)stack;
    void* old = (void*)idle_func;
    idle_func = func;
    idle_param = param;
    if (func && !idle_running) {
        memset(&idle_thread, 0, sizeof(idle_thread));
        idle_thread.prio = 32;
        idle_thread.used = 1;
        idle_thread.started = 1;
        LightEvent_Init(&idle_thread.wake, RESET_ONESHOT);
        u32 size = stack_size < 64 * 1024 ? 64 * 1024 : stack_size * 2;
        Thread th = threadCreate(idle_entry, 0, size, PRIO_IDLE, -2, true);
        if (!th) {
            __port_log("OSSetIdleFunction: cannot create the idle thread");
            svcBreak(USERBREAK_PANIC);
        }
        idle_running = 1;
    }
    return old;
}

/* ---- contexts, which only the crash handler uses ------------------------- */

void OSClearContext(void* ctx) { (void)ctx; }
void OSSetCurrentContext(void* ctx) { (void)ctx; }
void* OSGetCurrentContext(void) { return __port_self()->os_thread; }
void OSLoadContext(void* ctx) { (void)ctx; }
u32 OSGetStackPointer(void)
{
    u32 sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}
