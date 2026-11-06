/*
 * gs_thread.c - the GSthread switch primitives in C, on ARM coroutines.
 *
 * The decomp keeps these as the developers' assembly
 * (src/game/gs_thread_exact_800F0030.c, docs/asm_evidence/gs_thread.md):
 * threadSaveGPRRegisters and friends copy r0-r31/f0-f31 into a context block
 * (lbl_8047AC1C), threadExecute runs a task, and _threadSwitch hands control
 * back to the scheduler. The scheduler (fn_800F0A74) and GSthreadCreate are
 * C and are compiled from the decomp as they are.
 *
 * Context block, as GSthreadCreate fills it and the assembly reads it:
 *   +0x00..0x7C  gpr[0..31]; gpr[1] the stack (an offset into the task's
 *                GSmem stack, made absolute around threadExecute), gpr[3..10]
 *                the arguments (GSthreadSetArgs)
 *   +0x80        where the task resumes; for a new task its entry function
 *   +0x84        where the task returns to; for a new task the exit
 *                trampoline fn_800F0F4C, for the scheduler block fn_800F02F4
 *
 * Here each GSThread slot gets a coroutine with its own ARM stack. The task's
 * GSmem stack is still allocated by the game but not used: GSmem blocks can
 * move between runs, and ARM frames hold absolute addresses. A new task is
 * recognised by GSthreadCreate's call to threadSaveGPRRegisters, which
 * snapshots the registers into a fresh context block.
 */
#include "port/host.h"
#include "port/types.h"
#include <string.h>

typedef struct {
    u32 gpr[32];
    u32 lr;      /* +0x80 */
    u32 ctr;     /* +0x84 */
} GSThreadCtx;

extern u32 lbl_8047AC00;          /* current GSThread */
extern u32 lbl_8047AC1C;          /* context block for the save/load primitives */
extern u32 lbl_8047AC20;          /* context of the task to run */
extern u32 lbl_8047AC24;          /* scheduler context */
extern u8* lbl_8047AC28;          /* thread array */
extern u32 lbl_8047AC30;          /* thread count */

extern void __port_co_switch(void** save_sp, void* load_sp);
extern void __port_co_start(void);
extern void __port_log(const char* fmt, ...);

#define GSTHREAD_SIZE 0x24
#define MAX_COROUTINES 64
#define MIN_STACK (32 * 1024)

typedef struct {
    void* sp;
    unsigned char* stack;
    size_t stack_size;
    GSThreadCtx* ctx;      /* the context block it was started from */
    int live;
} Coroutine;

static Coroutine cos[MAX_COROUTINES];
static Coroutine* running;
static void* scheduler_sp;

/* Context blocks GSthreadCreate has just initialised. */
static GSThreadCtx* fresh[8];

static void mark_fresh(GSThreadCtx* ctx)
{
    for (int i = 0; i < 8; i++)
        if (fresh[i] == ctx)
            return;
    for (int i = 0; i < 8; i++)
        if (!fresh[i]) {
            fresh[i] = ctx;
            return;
        }
    fresh[0] = ctx;
}

static int take_fresh(GSThreadCtx* ctx)
{
    for (int i = 0; i < 8; i++)
        if (fresh[i] == ctx) {
            fresh[i] = 0;
            return 1;
        }
    return 0;
}

/* The register save/load primitives: only GSthreadCreate uses the save
 * half directly, to seed a new context; nothing reads the registers back
 * except the switch below. */
void threadSaveGPRRegisters(void)
{
    GSThreadCtx* ctx = (GSThreadCtx*)lbl_8047AC1C;
    memset(ctx->gpr, 0, sizeof(ctx->gpr));
    mark_fresh(ctx);
}

void threadSaveFPRRegisters(void) {}
void threadLoadGPRRegisters(void) {}
void threadLoadFPRRegisters(void) {}

/* Only its address is used: the scheduler stores it as the scheduler
 * block's return target. */
void fn_800F02F4(void) {}

typedef u32 (*TaskEntry)(u32, u32, u32, u32, u32, u32, u32, u32);
typedef void (*TaskExit)(u32);

void __port_co_main(Coroutine* co)
{
    GSThreadCtx* ctx = co->ctx;
    TaskEntry entry = (TaskEntry)ctx->lr;
    TaskExit exit_fn = (TaskExit)ctx->ctr;
    u32 result = entry(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                       ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    /* fn_800F0F4C records the result, unlinks the thread and switches away
     * for good. */
    exit_fn(result);
    co->live = 0;
    for (;;) {
        running = 0;
        __port_co_switch(&co->sp, scheduler_sp);
    }
}

static void start(Coroutine* co, GSThreadCtx* ctx, u32 stack_hint)
{
    size_t size = stack_hint * 2 > MIN_STACK ? stack_hint * 2 : MIN_STACK;
    size = (size + 7) & ~(size_t)7;
    if (co->stack && co->stack_size < size) {
        host_free(co->stack);
        co->stack = 0;
    }
    if (!co->stack) {
        co->stack = host_memalign(8, size);
        co->stack_size = size;
        if (!co->stack)
            host_break(0);
    }
    /* Frame popped by the first __port_co_switch: d8-d15 (16 words), then
     * r4-r11 and pc (9 words). r4 carries the record. */
    u32* sp = (u32*)(co->stack + co->stack_size) - 25;
    memset(sp, 0, 25 * sizeof(u32));
    sp[16] = (u32)co;                    /* r4 */
    sp[24] = (u32)__port_co_start;       /* pc */
    co->sp = sp;
    co->ctx = ctx;
    co->live = 1;
}

void threadExecute(void)
{
    GSThreadCtx* ctx = (GSThreadCtx*)lbl_8047AC20;
    u32 index = (lbl_8047AC00 - (u32)lbl_8047AC28) / GSTHREAD_SIZE;
    if (index >= MAX_COROUTINES || index >= lbl_8047AC30) {
        __port_log("threadExecute: thread index %lu out of range", index);
        host_break(0);
    }
    Coroutine* co = &cos[index];
    if (take_fresh(ctx) || !co->live) {
        u32 stack_size = *(u32*)((u8*)lbl_8047AC00 + 0x10);   /* GSThread.stackSize */
        start(co, ctx, stack_size);
    }
    co->ctx = ctx;              /* the block may have moved (GSmem) */
    running = co;
    __port_co_switch(&scheduler_sp, co->sp);
    running = 0;
}

/* Called from inside a task: give control back to the scheduler. What the
 * task passes in r3 is kept in its context and handed back when it resumes,
 * as the original does. */
u32 _threadSwitch(u32 r3)
{
    Coroutine* co = running;
    if (!co)
        return r3;              /* not inside a task */
    co->ctx->gpr[3] = r3;
    running = 0;
    __port_co_switch(&co->sp, scheduler_sp);
    return co->ctx->gpr[3];
}
