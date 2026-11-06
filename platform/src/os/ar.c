/*
 * ar.c - ARAM (the GameCube's 16 MB audio RAM) and its DMA queue.
 *
 * On the GameCube, ARAM is separate memory reached only through DMA. Here it
 * is a plain buffer and DMA is memcpy. Main-memory addresses arrive as u32;
 * pointers are 32 bits on the 3DS too, so they can be used directly.
 *
 * Transfers complete immediately. Completion callbacks, which the GameCube
 * ran from the DMA interrupt, are called before the request function
 * returns. That is the first thing to revisit if callers turn out to rely on
 * the callback running later.
 */
#include <3ds.h>
#include <malloc.h>
#include <string.h>

#define ARAM_SIZE 0x01000000u
#define ARAM_BASE 0x00004000u    /* the SDK reserves the first 16 KB */

typedef void (*ARCallback)(void);

static unsigned char* aram;
static ARCallback dma_callback;

static void* aram_ptr(unsigned long addr, unsigned long len)
{
    if (addr >= ARAM_SIZE || len > ARAM_SIZE - addr)
        svcBreak(USERBREAK_PANIC);
    return aram + addr;
}

unsigned long ARInit(unsigned long* stack, unsigned long entries)
{
    (void)stack; (void)entries;
    if (!aram) {
        aram = memalign(32, ARAM_SIZE);
        if (!aram)
            svcBreak(USERBREAK_PANIC);
        memset(aram, 0, ARAM_SIZE);
    }
    return ARAM_BASE;
}

unsigned long ARGetBaseAddress(void) { return ARAM_BASE; }
unsigned long ARGetSize(void) { return ARAM_SIZE; }
unsigned long ARGetDMAStatus(void) { return 0; }

ARCallback ARRegisterDMACallback(ARCallback cb)
{
    ARCallback old = dma_callback;
    dma_callback = cb;
    return old;
}

/* type 0: main memory to ARAM, type 1: ARAM to main memory */
void ARStartDMA(unsigned long type, unsigned long mram, unsigned long aram_addr, unsigned long len)
{
    if (type == 0)
        memcpy(aram_ptr(aram_addr, len), (void*)mram, len);
    else
        memcpy((void*)mram, aram_ptr(aram_addr, len), len);
    if (dma_callback)
        dma_callback();
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
    if (type == 0)
        memcpy(aram_ptr(dest, length), (void*)source, length);
    else
        memcpy((void*)dest, aram_ptr(source, length), length);
    if (callback)
        callback((unsigned long)req);
}
