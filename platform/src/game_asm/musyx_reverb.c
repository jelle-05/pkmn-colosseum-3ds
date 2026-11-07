/*
 * musyx_reverb.c - MusyX's reverb kernels, hand-written PowerPC assembly in
 * the original (src/musyx/runtime/reverb_candidate_80164520.c, admitted by
 * the decomp's docs/asm_evidence/musyx_reverb.md).
 *
 * They process the auxiliary buffers the DSP mixes into, from the aux
 * callback (ReverbHICallback): DoCrossTalk mixes the two channels into each
 * other, HandleReverb runs the comb and all-pass filters of one channel.
 * The DSP is silent until the audio backend exists (audio/dsp.c), so these
 * have nothing to process and do nothing. With the backend they get C
 * versions, after the reference MusyX runtime (AxioDL/musyx, reverb_fx.c).
 */
typedef int s32;
typedef float f32;
typedef struct SND_AUX_REVERBHI SND_AUX_REVERBHI;

void DoCrossTalk(s32* a, s32* b, f32 start, f32 end)
{
    (void)a; (void)b; (void)start; (void)end;
}

void HandleReverb(s32* buffer, SND_AUX_REVERBHI* rev, s32 channel)
{
    (void)buffer; (void)rev; (void)channel;
}
