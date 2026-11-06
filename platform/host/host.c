/*
 * host.c - the little-endian host: libctru, newlib and the entry point.
 *
 * Everything here runs in the ARM11's normal little-endian data mode. The
 * big-endian image (game and platform layer) calls the functions listed
 * in port/host.h through switching stubs; see docs/BIG_ENDIAN.md. Values
 * this file stores into big-endian memory go through be_store32.
 */
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"

/* Runs fn(arg) in big-endian mode (host_switch.S). */
extern void host_be_call(host_entry fn, void* arg);

static void be_store32(host_u32* p, host_u32 v) { *p = __builtin_bswap32(v); }

/* ---- process -------------------------------------------------------------------- */

int host_main_loop(void) { return aptMainLoop(); }
void host_exit(int code) { exit(code); }
void host_break(int reason) { svcBreak((UserBreakType)reason); }
void host_debug_string(const char* s, host_u32 len) { svcOutputDebugString(s, (s32)len); }

/* osGetTime counts milliseconds from 1900-01-01; the GameCube counts bus
 * clock / 4 ticks (40.5 MHz) from 2000-01-01. The system tick (268 MHz)
 * gives the finer part. */
#define GC_TICKS_PER_SEC 40500000ull
#define MS_1900_TO_2000 3155673600000ull

void host_gc_time(host_u32* out)
{
    static u64 base_ms, base_tick;
    if (!base_ms) {
        base_ms = osGetTime();
        base_tick = svcGetSystemTick();
    }
    u64 dt = svcGetSystemTick() - base_tick;               /* SYSCLOCK_ARM11 ticks */
    u64 ms = base_ms - MS_1900_TO_2000;
    u64 gc = ms * (GC_TICKS_PER_SEC / 1000) + dt / SYSCLOCK_ARM11 * GC_TICKS_PER_SEC
             + (dt % SYSCLOCK_ARM11) * GC_TICKS_PER_SEC / SYSCLOCK_ARM11;
    be_store32(&out[0], (host_u32)(gc >> 32));
    be_store32(&out[1], (host_u32)gc);
}

void host_sleep_us(host_u32 us) { svcSleepThread((s64)us * 1000); }

/* ---- threads -------------------------------------------------------------------- */

typedef struct { host_entry fn; void* arg; } Start;

static void thread_start(void* p)
{
    Start s = *(Start*)p;
    free(p);
    host_be_call(s.fn, s.arg);
}

host_thread host_thread_create(host_entry fn, void* arg, host_u32 stack, host_u32 prio_detached)
{
    Start* s = malloc(sizeof(*s));
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    Thread t = threadCreate(thread_start, s, stack, (int)(prio_detached & 0xFF), -2, (prio_detached >> 8) & 1);
    if (!t)
        free(s);
    return t;
}

host_thread host_thread_current(void) { return threadGetCurrent(); }
host_u32 host_thread_handle(host_thread t) { return t ? threadGetHandle((Thread)t) : CUR_THREAD_HANDLE; }
void host_thread_exit(int code) { threadExit(code); }
void host_thread_join(host_thread t) { threadJoin((Thread)t, U64_MAX); }
int host_thread_set_priority(host_u32 handle, int prio) { return svcSetThreadPriority(handle, prio); }

host_u32 host_dup_handle(host_u32 handle)
{
    Handle out = 0;
    svcDuplicateHandle(&out, handle);
    return out;
}

/* ---- sync ----------------------------------------------------------------------- */

void host_lock_init(host_lock* l) { LightLock_Init((LightLock*)l); }
void host_lock_lock(host_lock* l) { LightLock_Lock((LightLock*)l); }
void host_lock_unlock(host_lock* l) { LightLock_Unlock((LightLock*)l); }
void host_rlock_init(host_rlock* l) { RecursiveLock_Init((RecursiveLock*)l); }
void host_rlock_lock(host_rlock* l) { RecursiveLock_Lock((RecursiveLock*)l); }
void host_rlock_unlock(host_rlock* l) { RecursiveLock_Unlock((RecursiveLock*)l); }
int host_rlock_trylock(host_rlock* l) { return RecursiveLock_TryLock((RecursiveLock*)l) == 0; }
void host_event_init(host_event* e, int oneshot)
{
    LightEvent_Init((LightEvent*)e, oneshot ? RESET_ONESHOT : RESET_STICKY);
}
void host_event_signal(host_event* e) { LightEvent_Signal((LightEvent*)e); }
void host_event_clear(host_event* e) { LightEvent_Clear((LightEvent*)e); }
void host_event_wait(host_event* e) { LightEvent_Wait((LightEvent*)e); }
int host_event_wait_us(host_event* e, host_u32 us)
{
    return LightEvent_WaitTimeout((LightEvent*)e, (s64)us * 1000) != 0;
}

_Static_assert(sizeof(host_lock) == sizeof(LightLock), "host_lock");
_Static_assert(sizeof(host_rlock) == sizeof(RecursiveLock), "host_rlock");
_Static_assert(sizeof(host_event) == sizeof(LightEvent), "host_event");

/* ---- memory --------------------------------------------------------------------- */

void* host_malloc(host_u32 size) { return malloc(size); }
void* host_memalign(host_u32 align, host_u32 size) { return memalign(align, size); }
void host_free(void* p) { free(p); }
void* host_realloc(void* p, host_u32 size) { return realloc(p, size); }

/* ---- files ---------------------------------------------------------------------- */

#define MAX_FILES 16
static FILE* files[MAX_FILES];

int host_file_open(const char* path, const char* mode)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i]) {
            FILE* f = fopen(path, mode);
            if (!f)
                return -1;
            if (mode[0] == 'r')
                setvbuf(f, NULL, _IOFBF, 64 * 1024);
            files[i] = f;
            return i;
        }
    }
    return -1;
}

host_u32 host_file_size(int fd)
{
    if (fd < 0 || fd >= MAX_FILES || !files[fd])
        return 0;
    long pos = ftell(files[fd]);
    fseek(files[fd], 0, SEEK_END);
    long size = ftell(files[fd]);
    fseek(files[fd], pos, SEEK_SET);
    return (host_u32)size;
}

host_u32 host_file_read(int fd, void* buf, host_u32 size, host_u32 offset)
{
    if (fd < 0 || fd >= MAX_FILES || !files[fd])
        return 0;
    if (fseeko(files[fd], (off_t)offset, SEEK_SET))
        return 0;
    return (host_u32)fread(buf, 1, size, files[fd]);
}

host_u32 host_file_write(int fd, const void* buf, host_u32 size)
{
    if (fd < 0 || fd >= MAX_FILES || !files[fd])
        return 0;
    return (host_u32)fwrite(buf, 1, size, files[fd]);
}

void host_file_close(int fd)
{
    if (fd < 0 || fd >= MAX_FILES || !files[fd])
        return;
    fclose(files[fd]);
    files[fd] = 0;
}

/* ---- math ----------------------------------------------------------------------- */

float host_sinf(float x) { return sinf(x); }
float host_cosf(float x) { return cosf(x); }
float host_tanf(float x) { return tanf(x); }

/* ---- input ---------------------------------------------------------------------- */

void host_input_init(void) { irrstInit(); }

host_u32 host_input_scan(void)
{
    hidScanInput();
    return hidKeysHeld();
}

host_u32 host_input_circle(void)
{
    circlePosition cp;
    hidCircleRead(&cp);
    return (host_u32)(u16)cp.dx | (host_u32)(u16)cp.dy << 16;
}

host_u32 host_input_cstick(void)
{
    circlePosition cs;
    irrstCstickRead(&cs);
    return (host_u32)(u16)cs.dx | (host_u32)(u16)cs.dy << 16;
}

/* ---- entry ---------------------------------------------------------------------- */

/* Defined by the big-endian image; addresses given to this link by
 * tools/link_be.py. */
extern host_u32 __host_table[];
extern void __be_start(void* unused);

/* The big-endian stubs (platform/src/rt/host_stubs.S) jump through this
 * table, in HOST_FUNCTIONS order; they read it big-endian. */
static void fill_table(host_u32* t)
{
    int i = 0;
#define FILL(name, ret, params) be_store32(&t[i++], (host_u32)&name);
    HOST_FUNCTIONS(FILL)
    HOST_LIBM(FILL)
#undef FILL
}

int main(void)
{
    fill_table(__host_table);
    host_be_call(__be_start, 0);
    return 0;
}
