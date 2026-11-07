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
#include <string.h>

#include "os_thread.h"

extern void __port_log(const char* fmt, ...);

#define MAX_THREADS 32
#define OSTHREAD_SIZE 0x318

static PortThread threads[MAX_THREADS];
static PortThread main_thread, idle_thread;
static unsigned char default_os_thread[OSTHREAD_SIZE] __attribute__((aligned(8)));
static host_lock sched_lock;
static s32 scheduler_disabled;

/* Which PortThread a 3DS thread is. Thread-local storage would live in
 * libctru's (little-endian) thread block, so this is a table instead; the
 * main thread is the one host_thread_current() reports as NULL. Threads the
 * platform starts for itself (interrupts, vblank) bind themselves, or get
 * a record of their own on first use. */
#define MAX_BINDINGS 48
static struct { host_thread thread; PortThread* port; } bindings[MAX_BINDINGS];
static PortThread anon[8];
static int n_anon;

void __port_set_self(PortThread* t)
{
    host_thread cur = host_thread_current();
    if (!cur)
        return;
    if (!t->handle)
        t->handle = host_dup_handle(host_thread_handle(0));
    host_lock_lock(&sched_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_BINDINGS; i++) {
        if (bindings[i].thread == cur) {
            bindings[i].port = t;
            free_slot = -2;
            break;
        }
        if (!bindings[i].thread && free_slot == -1)
            free_slot = i;
    }
    if (free_slot >= 0) {
        bindings[free_slot].thread = cur;
        bindings[free_slot].port = t;
    }
    host_lock_unlock(&sched_lock);
}

static void unbind_self(void)
{
    host_thread cur = host_thread_current();
    host_lock_lock(&sched_lock);
    for (int i = 0; i < MAX_BINDINGS; i++)
        if (bindings[i].thread == cur)
            bindings[i].thread = 0;
    host_lock_unlock(&sched_lock);
}

PortThread* __port_self(void)
{
    host_thread cur = host_thread_current();
    if (!cur)
        return &main_thread;
    for (int i = 0; i < MAX_BINDINGS; i++)
        if (bindings[i].thread == cur)
            return bindings[i].port;
    PortThread* t = n_anon < 8 ? &anon[n_anon++] : &anon[7];
    t->used = 1;
    t->started = 1;
    t->prio = 0;
    t->prio3ds = PRIO_IRQ;           /* the platform's own threads */
    host_event_init(&t->wake, 1);
    __port_set_self(t);
    return t;
}

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
    host_lock_init(&sched_lock);
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.os_thread = default_os_thread;
    main_thread.prio = 16;                       /* the SDK's default thread */
    main_thread.started = 1;
    main_thread.used = 1;
    host_event_init(&main_thread.wake, 1);
    main_thread.handle = host_dup_handle(host_thread_handle(0));
    main_thread.prio3ds = prio_3ds(main_thread.prio);
    host_thread_set_priority(main_thread.handle, prio_3ds(main_thread.prio));
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
        __port_irq_drop(t);
    }
    set_state(t, OS_THREAD_STATE_WAITING);
    host_event_wait(&t->wake);
    set_state(t, OS_THREAD_STATE_RUNNING);
    if (had) {
        __port_irq_take(t, __builtin_return_address(0));
    }
}

static void sleep_on(OSThreadQueue* q, void* from)
{
    PortThread* t = __port_self();
    host_lock_lock(&sched_lock);
    t->waiting_on = q;
    t->wait_from = from;
    t->qnext = 0;
    if (q->tail)
        q->tail->qnext = t;
    else
        q->head = t;
    q->tail = t;
    host_lock_unlock(&sched_lock);
    block(t);
    t->wait_from = 0;
}

void __port_sleep_on(OSThreadQueue* q) { sleep_on(q, __builtin_return_address(0)); }

/* Heartbeat for the log: what every thread is doing. Called from the vblank
   thread; reads without locking, good enough for a diagnosis. */
void __port_thread_dump(void)
{
    extern volatile int __port_irq_state;
    extern void* volatile __port_irq_handler;
    extern volatile unsigned __port_irq_posted, __port_irq_run, __port_irq_takes;
    __port_log("  irq thread: state %d, handler %p, posted %u, run %u; lock taken %u times",
               __port_irq_state, __port_irq_handler, __port_irq_posted, __port_irq_run, __port_irq_takes);
    PortThread* owner = __port_irq_owner;
    __port_log("  interrupts off: %s %p (taken at %p)", owner ? "thread" : "nobody",
               owner ? owner->os_thread : 0, __port_irq_taken_at);
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
    host_lock_lock(&sched_lock);
    PortThread* t = q->head;
    q->head = q->tail = 0;
    while (t) {
        PortThread* next = t->qnext;
        t->qnext = 0;
        t->waiting_on = 0;
        host_event_signal(&t->wake);
        t = next;
    }
    host_lock_unlock(&sched_lock);
}

void OSSleepThread(OSThreadQueue* q) { sleep_on(q, __builtin_return_address(0)); }
void OSWakeupThread(OSThreadQueue* q) { __port_wakeup(q); }

/* ---- thread lifetime ------------------------------------------------------------ */

void OSExitThread(void* result);

static void entry(void* arg)
{
    PortThread* t = arg;
    t->thread = host_thread_current();
    __port_set_self(t);
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
    t->prio3ds = prio_3ds(prio);
    t->suspend = 1;                     /* created suspended, as the SDK does */
    t->detached = (attr & 1) != 0;      /* OS_THREAD_ATTR_DETACH */
    host_event_init(&t->wake, 1);
    set_state(t, OS_THREAD_STATE_READY);
    return 1;
}

s32 OSResumeThread(void* os_thread)
{
    PortThread* t = find(os_thread);
    if (!t)
        return 0;
    host_lock_lock(&sched_lock);
    s32 prev = t->suspend;
    if (t->suspend > 0)
        t->suspend--;
    int go = t->suspend == 0 && prev > 0;
    host_lock_unlock(&sched_lock);
    if (go) {
        if (!t->started) {
            t->started = 1;
            u32 size = t->stack_size < 64 * 1024 ? 64 * 1024 : t->stack_size * 2;
            t->thread = host_thread_create(entry, t, size, prio_3ds(t->prio));
            if (!t->thread) {
                __port_log("OSResumeThread: cannot create a thread");
                host_break(0);
            }
            t->handle = host_thread_handle(t->thread);
        } else {
            host_event_signal(&t->wake);
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
    host_lock_lock(&sched_lock);
    s32 prev = t->suspend++;
    host_lock_unlock(&sched_lock);
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
    if (t->thread) {
        unbind_self();
        host_thread_exit(0);
    }
    /* the main thread exiting ends the program */
    for (;;)
        host_sleep_us(1000000);
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
    t->prio3ds = prio_3ds(prio);
    if (t->handle && !t->boosted)
        host_thread_set_priority(t->handle, t->prio3ds);
    return 1;
}

s32 OSGetThreadPriority(void* os_thread)
{
    PortThread* t = find(os_thread);
    return t ? t->prio : 0;
}

/* The interrupt state is per thread on the GameCube: a thread that yields
 * with interrupts off does not keep them off for the others. */
void OSYieldThread(void)
{
    PortThread* t = __port_self();
    int had = t->irq_off;
    if (had) {
        __port_irq_drop(t);
    }
    host_sleep_us(0);
    if (had) {
        __port_irq_take(t, __builtin_return_address(0));
    }
}

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
    idle_thread.thread = host_thread_current();
    __port_set_self(&idle_thread);
    for (;;) {
        if (idle_func)
            idle_func(idle_param);
        host_sleep_us(1000);      /* only if the function ever returns */
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
        idle_thread.prio3ds = PRIO_IDLE;
        idle_thread.used = 1;
        idle_thread.started = 1;
        host_event_init(&idle_thread.wake, 1);
        u32 size = stack_size < 64 * 1024 ? 64 * 1024 : stack_size * 2;
        host_thread th = host_thread_create(idle_entry, 0, size, PRIO_IDLE | 1 << 8);
        if (!th) {
            __port_log("OSSetIdleFunction: cannot create the idle thread");
            host_break(0);
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
