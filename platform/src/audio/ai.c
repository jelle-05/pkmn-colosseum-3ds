/*
 * ai.c - the audio interface (AI): the DMA that feeds mixed samples to the
 * DAC, and the disc-streaming volume controls.
 *
 * Placeholder until the audio backend exists (roadmap phase 6). AIInit and
 * the register-style setters are accepted and remembered; nothing is played.
 * MusyX drives the AI DMA from its own callback, which is why the callback
 * is kept: the NDSP backend will call it once per audio frame.
 */

typedef void (*AIDCallback)(void);

static AIDCallback dma_callback;
static unsigned long dma_start, dma_length;
static int dma_running;
static unsigned long dsp_sample_rate = 1;    /* AI_SAMPLERATE_48KHZ */

void AIInit(unsigned char* stack) { (void)stack; }

AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
    AIDCallback old = dma_callback;
    dma_callback = cb;
    return old;
}

void AIInitDMA(unsigned long start, unsigned long length)
{
    dma_start = start;
    dma_length = length;
}

void AIStartDMA(void) { dma_running = 1; }
void AIStopDMA(void) { dma_running = 0; }

unsigned long AIGetDSPSampleRate(void) { return dsp_sample_rate; }
void AISetDSPSampleRate(unsigned long rate) { dsp_sample_rate = rate; }
