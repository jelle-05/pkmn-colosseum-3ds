/*
 * thp.c - the THP movie player, without video for now.
 *
 * The game plays THP movies (MJPEG video with ADPCM audio) for the
 * Genius Sonority logo, the opening and the credits. Decoding them on the
 * 3DS comes later (roadmap phase 7, either a decoder or transcoding the
 * files). Until then the player initialises but every movie fails to open.
 * The game handles that: GSmovieOpen's loader thread logs
 * "Fail to open the thp file", reports the movie as done, and the game
 * carries on as if it had ended.
 */
#include <string.h>

typedef unsigned long u32;
typedef long s32;

enum { THP_STATE_STOP = 0 };

int THPPlayerInit(void) { return 1; }
void THPPlayerQuit(void) {}
int THPPlayerOpen(const char* path, int on_memory) { (void)path; (void)on_memory; return 0; }
int THPPlayerClose(void) { return 1; }
u32 THPPlayerCalcNeedMemory(void) { return 0; }
int THPPlayerSetBuffer(void* buffer) { (void)buffer; return 1; }
int THPPlayerPrepare(s32 frame, s32 play_flag, s32 audio_track)
{
    (void)frame; (void)play_flag; (void)audio_track;
    return 0;
}
int THPPlayerPlay(void) { return 0; }
void THPPlayerStop(void) {}
int THPPlayerPause(void) { return 0; }
s32 THPPlayerGetState(void) { return THP_STATE_STOP; }
s32 THPPlayerGetFrame(void) { return 0; }
s32 THPPlayerDrawCurrentFrame(void* rmode, u32 x, u32 y, u32 w, u32 h)
{
    (void)rmode; (void)x; (void)y; (void)w; (void)h;
    return -1;
}
void THPPlayerDrawDone(void) {}
int THPPlayerGetVideoInfo(void* info)
{
    memset(info, 0, 12);
    return 0;
}
int THPPlayerGetAudioInfo(void* info)
{
    memset(info, 0, 16);
    return 0;
}
int THPInit(void) { return 1; }
