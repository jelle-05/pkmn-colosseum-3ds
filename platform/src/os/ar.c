/*
 * ar.c - ARAM (the GameCube's 16 MB audio RAM) and its DMA queue.
 *
 * On the GameCube, ARAM is separate memory reached only through DMA. Here it
 * is a plain buffer and DMA is memcpy. Main-memory addresses arrive as u32;
 * pointers are 32 bits on the 3DS too, so they can be used directly.
 *
 * A transfer completes shortly after it is requested: the copy is made and
 * the callback run from the interrupt thread, as the GameCube runs it from
 * the DMA interrupt. Callers rely on that order: they post a request, then
 * record that they wait for it (fsys's ARAM cache does), so a callback that
 * ran inside the request would be overwritten.
 */
#include "port/host.h"
#include <string.h>

#include "os_thread.h"

extern long long OSGetTime(void);

/* DMA from the interrupt thread, a moment after the request. */
#define DMA_LATENCY_TICKS (40500000 / 20000)       /* 50 us */

#define ARAM_SIZE 0x01000000u
#define ARAM_BASE 0x00004000u    /* the SDK reserves the first 16 KB */

typedef void (*ARCallback)(void);

static unsigned char* aram;
static ARCallback dma_callback;

static void* aram_ptr(unsigned long addr, unsigned long len)
{
    if (addr >= ARAM_SIZE || len > ARAM_SIZE - addr)
        host_break(0);
    return aram + addr;
}

unsigned long ARInit(unsigned long* stack, unsigned long entries)
{
    (void)stack; (void)entries;
    if (!aram) {
        aram = host_memalign(32, ARAM_SIZE);
        if (!aram)
            host_break(0);
        memset(aram, 0, ARAM_SIZE);
    }
    return ARAM_BASE;
}

unsigned long ARGetBaseAddress(void) { return ARAM_BASE; }
unsigned long ARGetSize(void) { return ARAM_SIZE; }
/* One AR DMA at a time, as on the GameCube; ARGetDMAStatus is busy until
 * it has completed. */
static struct { unsigned long type, mram, aram, len; } ar_dma;
static volatile int ar_busy;

unsigned long ARGetDMAStatus(void) { return ar_busy; }

ARCallback ARRegisterDMACallback(ARCallback cb)
{
    ARCallback old = dma_callback;
    dma_callback = cb;
    return old;
}

static void copy(unsigned long type, unsigned long mram, unsigned long aram_addr, unsigned long len)
{
    /* type 0: main memory to ARAM, type 1: ARAM to main memory */
    if (type == 0)
        memcpy(aram_ptr(aram_addr, len), (void*)mram, len);
    else
        memcpy((void*)mram, aram_ptr(aram_addr, len), len);
}

static void ar_dma_done(void* unused, long unused2)
{
    (void)unused; (void)unused2;
    copy(ar_dma.type, ar_dma.mram, ar_dma.aram, ar_dma.len);
    ar_busy = 0;
    if (dma_callback)
        dma_callback();
}

void ARStartDMA(unsigned long type, unsigned long mram, unsigned long aram_addr, unsigned long len)
{
    ar_dma.type = type;
    ar_dma.mram = mram;
    ar_dma.aram = aram_addr;
    ar_dma.len = len;
    ar_busy = 1;
    __port_irq_post(OSGetTime() + DMA_LATENCY_TICKS, ar_dma_done, 0, 0);
}

/* ---- ARQ: the SDK's queue on top of ARAM DMA ---------------------------- */

typedef struct ARQRequest ARQRequest;
typedef void (*ARQCallback)(unsigned long request);

struct ARQRequest {
    ARQRequest* next;
    unsigned long owner;
    unsigned long type;
    unsigned long priority;
    unsigned long source;
    unsigned long dest;
    unsigned long length;
    ARQCallback callback;
};

void ARQInit(void) {}

static void arq_done(void* r, long unused)
{
    (void)unused;
    ARQRequest* req = r;
    copy(req->type, req->type == 0 ? req->source : req->dest, req->type == 0 ? req->dest : req->source,
         req->length);
    if (req->callback)
        req->callback((unsigned long)req);
}

unsigned long ARQGetChunkSize(void) { return 4096; }

void ARQPostRequest(ARQRequest* req, unsigned long owner, unsigned long type, unsigned long priority,
                    unsigned long source, unsigned long dest, unsigned long length, ARQCallback callback)
{
    req->next = 0;
    req->owner = owner;
    req->type = type;
    req->priority = priority;
    req->source = source;
    req->dest = dest;
    req->length = length;
    req->callback = callback;
    __port_irq_post(OSGetTime() + DMA_LATENCY_TICKS, arq_done, req, 0);
}
