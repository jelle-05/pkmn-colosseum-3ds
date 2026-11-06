/*
 * lc.c - the Gekko's locked cache.
 *
 * On the GameCube, half of the 32 KB L1 data cache can be locked and used as
 * 16 KB of fast scratch memory at 0xE0000000, filled and drained by a small
 * DMA engine. Here it is a plain buffer and the DMA is memcpy, done at once.
 * Game code reaches it through the scratch base GSscratchInit stores, which
 * now points at this buffer (see platform/src/game_asm/gs_scratch.c).
 */
#include <string.h>

#define LC_SIZE 0x4000u

static unsigned char locked_cache[LC_SIZE] __attribute__((aligned(32)));

unsigned char* __port_locked_cache(void) { return locked_cache; }

void LCEnable(void) {}
void LCDisable(void) {}

/* dest in the locked cache, src in main memory */
void LCLoadData(void* dest, void* src, unsigned long n) { memcpy(dest, src, n); }
/* dest in main memory, src in the locked cache */
void LCStoreData(void* dest, void* src, unsigned long n) { memcpy(dest, src, n); }

unsigned long LCQueueLength(void) { return 0; }
void LCQueueWait(unsigned long len) { (void)len; }
