/*
 * ai.c - the audio interface (AI): the DMA that feeds mixed samples to the
 * DAC, and the disc-streaming volume controls.
 *
 * The GameCube's AI plays a buffer of 16-bit stereo samples by DMA and
 * interrupts when it has started on it, so the program can queue the next.
 * MusyX runs its whole synth from that interrupt (salCallback): every 0x280
 * bytes, 5 ms at 32 kHz. Here the DMA is a clock on the interrupt thread
 * with the same period, so music and sound effects keep their timing.
 * Nothing is played yet; the samples will go to NDSP once the audio
 * backend exists (roadmap phase 6).
 */
#include "../os/os_thread.h"

extern long long OSGetTime(void);

typedef void (*AIDCallback)(void);

#define GC_TICKS_PER_SEC 40500000ll
#define AI_RATE 32000                /* MusyX mixes at 32 kHz */

static AIDCallback dma_callback;
static unsigned long dma_start, dma_length;
static int dma_running;
static unsigned long dsp_sample_rate = 1;    /* AI_SAMPLERATE_48KHZ */
static long long next_due;

void AIInit(unsigned char* stack) { (void)stack; }

AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
    int level = OSDisableInterrupts();
    AIDCallback old = dma_callback;
    dma_callback = cb;
    OSRestoreInterrupts(level);
    return old;
}

void AIInitDMA(unsigned long start, unsigned long length)
{
    dma_start = start;
    dma_length = length;
}

/* Duration of one buffer: 4 bytes per stereo sample. */
static long long buffer_ticks(void)
{
    unsigned long len = dma_length ? dma_length : 0x280;
    return (long long)(len / 4) * GC_TICKS_PER_SEC / AI_RATE;
}

static void dma_interrupt(void* unused, long unused2)
{
    (void)unused; (void)unused2;
    if (!dma_running)
        return;
    /* Schedule the next before running the callback, which takes a while;
     * the period stays steady, as with the hardware. */
    next_due += buffer_ticks();
    long long now = OSGetTime();
    if (next_due < now - 20 * buffer_ticks())
        next_due = now;                      /* far behind: drop, do not catch up */
    __port_irq_post(next_due, dma_interrupt, 0, 0);
    if (dma_callback)
        dma_callback();
}

void AIStartDMA(void)
{
    int level = OSDisableInterrupts();
    if (!dma_running) {
        dma_running = 1;
        next_due = OSGetTime() + buffer_ticks();
        __port_irq_post(next_due, dma_interrupt, 0, 0);
    }
    OSRestoreInterrupts(level);
}

void AIStopDMA(void)
{
    int level = OSDisableInterrupts();
    dma_running = 0;
    __port_irq_cancel(dma_interrupt, 0);
    OSRestoreInterrupts(level);
}

unsigned long AIGetDSPSampleRate(void) { return dsp_sample_rate; }
void AISetDSPSampleRate(unsigned long rate) { dsp_sample_rate = rate; }
