/*
 * dsp.c - a silent DSP.
 *
 * On the GameCube, MusyX runs a mixer on the audio DSP. The CPU loads the
 * microcode as a DSP task (DSPAddTask), waits for the task's init callback,
 * and from then on, every audio frame (5 ms), builds a command list and
 * mails it to the DSP: first 0xBABE0000 | size, then the list's address.
 * The DSP mixes into the AI buffer and the task's resume callback reports
 * the frame done (MusyX's hw_dolphin.c: salStartDsp, dspResumeCallback).
 *
 * Until the audio backend exists, this DSP accepts the task and the mail
 * and answers as the real one would, from the interrupt thread, without
 * mixing anything. MusyX keeps its timing (the AI DMA callback in ai.c
 * drives it), and the game hears silence.
 */
#include "port/types.h"
#include "../os/os_thread.h"

extern long long OSGetTime(void);

typedef void (*DSPCallback)(void* task);

/* The SDK's DSPTaskInfo, as MusyX fills it. */
typedef struct DSPTaskInfo {
    volatile u32 state;
    volatile u32 priority;
    volatile u32 flags;
    u16* iram_mmem_addr;
    u32 iram_length;
    u32 iram_addr;
    u16* dram_mmem_addr;
    u32 dram_length;
    u32 dram_addr;
    u16 dsp_init_vector;
    u16 dsp_resume_vector;
    DSPCallback init_cb;
    DSPCallback res_cb;
    DSPCallback done_cb;
    DSPCallback req_cb;
    struct DSPTaskInfo* next;
    struct DSPTaskInfo* prev;
    s64 t_context;
    s64 t_task;
} DSPTaskInfo;

enum { DSP_TASK_STATE_INIT = 0, DSP_TASK_STATE_RUN = 1 };

/* A DSP frame takes a fraction of the 5 ms audio frame on the GameCube. */
#define DSP_LATENCY_TICKS (40500000 / 4000)          /* 0.25 ms */

static DSPTaskInfo* task;
static int mail_count;      /* mails of the current command list */

static void call_init(void* t, long unused)
{
    (void)unused;
    DSPTaskInfo* ti = t;
    ti->state = DSP_TASK_STATE_RUN;
    if (ti->init_cb)
        ti->init_cb(ti);
}

static void call_resume(void* t, long unused)
{
    (void)unused;
    DSPTaskInfo* ti = t;
    if (ti->res_cb)
        ti->res_cb(ti);
}

void DSPInit(void) {}
void DSPReset(void) {}
void DSPHalt(void) {}
u32 DSPGetDMAStatus(void) { return 0; }

DSPTaskInfo* DSPAddTask(DSPTaskInfo* t)
{
    task = t;
    t->state = DSP_TASK_STATE_INIT;
    t->next = t->prev = 0;
    mail_count = 0;
    __port_irq_post(OSGetTime() + DSP_LATENCY_TICKS, call_init, t, 0);
    return t;
}

/* The mailbox is always free: the mail is taken at once. */
u32 fn_800AE794(void) { return 0; }                   /* DSPCheckMailToDSP */
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }

void DSPSendMailToDSP(u32 mail)
{
    if ((mail >> 16) == 0xBABE) {
        mail_count = 1;                                /* the list's size */
        return;
    }
    if (mail_count == 1) {
        mail_count = 0;                                /* the list's address */
        if (task)
            __port_irq_post(OSGetTime() + DSP_LATENCY_TICKS, call_resume, task, 0);
    }
}
