/*
 * dvd.c - the Dolphin DVD library on top of the game's own disc image.
 *
 * The user's GC6E01 ISO lives on the SD card. DVDInit reads its header and
 * file system table (FST) the same way the SDK does, so paths, entry numbers
 * and file offsets mean what they meant on the GameCube: a DVDFileInfo's
 * startAddr is the file's offset in the image, and DVDRead reads from there.
 *
 * Asynchronous requests: the data is read at once, but completion (the
 * command block's state and the callback) arrives later as an interrupt,
 * the way the GameCube delivered it. Game code sets up its own state after
 * starting a request, so calling back before the request function returns
 * would break it.
 */
#include "port/prof.h"
#include "port/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "port/gc_mem.h"

#define ISO_PATH "sdmc:/3ds/colosseum/GC6E01.iso"

/* ---- types, as in the decomp's include/dolphin/dvd/dvd.h ---------------- */

typedef struct DVDCommandBlock DVDCommandBlock;
typedef void (*DVDCBCallback)(long result, DVDCommandBlock* block);

struct DVDCommandBlock {
    DVDCommandBlock* next;
    DVDCommandBlock* prev;
    unsigned long command;
    long state;
    unsigned long offset;
    unsigned long length;
    void* addr;
    unsigned long currTransferSize;
    unsigned long transferredSize;
    void* id;
    DVDCBCallback callback;
    void* userData;
};

typedef struct DVDFileInfo {
    DVDCommandBlock cb;
    unsigned long startAddr;
    unsigned long length;
    DVDCBCallback callback;
} DVDFileInfo;

enum {
    DVD_STATE_END = 0,
    DVD_STATE_BUSY = 1,
    DVD_STATE_FATAL_ERROR = -1,
};
enum { DVD_COMMAND_READ = 1, DVD_COMMAND_SEEK = 2 };


/* ---- the image and its FST ---------------------------------------------- */

typedef struct {
    unsigned long name_or_flags;   /* bit 24: directory; low 24 bits: name offset */
    unsigned long offset_or_parent;
    unsigned long length_or_next;
} FSTEntry;

static int iso = -1;              /* host file */
static FSTEntry* fst;
static unsigned long fst_count;
static const char* fst_names;
static long current_dir;           /* entry number of the current directory */

static unsigned long be32(const unsigned char* p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static void fatal(const char* what)
{
    char line[128];
    extern void __port_fatal(const char*);
    snprintf(line, sizeof(line), "DVD: %s", what);
    __port_fatal(line);
}

static int read_image(void* dst, unsigned long offset, unsigned long len)
{
    PROF_BEGIN(t_read);
    int ok = host_file_read(iso, dst, len, offset) == len;
    PROF_END(PROF_DVD, t_read);
    return ok;
}

void DVDInit(void)
{
    if (iso >= 0)
        return;
    iso = host_file_open(ISO_PATH, "rb");
    if (iso < 0)
        fatal("cannot open " ISO_PATH);

    unsigned char header[0x440];
    if (!read_image(header, 0, sizeof(header)))
        fatal("cannot read the disc header");
    if (memcmp(header, "GC6E01", 6) != 0)
        fatal("not a GC6E01 disc image");

    /* The SDK keeps the disc ID at the start of low memory. */
    memcpy(__gc_lowmem, header, 0x20);

    unsigned long fst_off = be32(header + 0x424), fst_size = be32(header + 0x428);
    unsigned char* raw = host_malloc(fst_size);
    if (!raw || !read_image(raw, fst_off, fst_size))
        fatal("cannot read the FST");
    fst_count = be32(raw + 8);
    fst = host_malloc(fst_count * sizeof(FSTEntry));
    if (!fst)
        fatal("out of memory");
    for (unsigned long i = 0; i < fst_count; i++) {
        fst[i].name_or_flags = be32(raw + i * 12);
        fst[i].offset_or_parent = be32(raw + i * 12 + 4);
        fst[i].length_or_next = be32(raw + i * 12 + 8);
    }
    fst_names = (const char*)raw + fst_count * 12;    /* raw stays allocated */
    current_dir = 0;
}

static int is_dir(unsigned long e) { return (fst[e].name_or_flags >> 24) != 0; }
static const char* entry_name(unsigned long e) { return fst_names + (fst[e].name_or_flags & 0xFFFFFF); }

/* GameCube path rules: '/' separated, case-insensitive, "." and "..",
 * relative to the current directory unless it starts with '/'. */
long DVDConvertPathToEntrynum(const char* path)
{
    long dir = current_dir;
    if (!fst)
        return -1;
    if (*path == '/') {
        dir = 0;
        path++;
    }
    while (*path) {
        const char* end = path;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - path);
        if (len == 1 && path[0] == '.') {
            /* stay */
        } else if (len == 2 && path[0] == '.' && path[1] == '.') {
            dir = (long)fst[dir].offset_or_parent;
        } else {
            unsigned long i = (unsigned long)dir + 1, stop = fst[dir].length_or_next;
            long found = -1;
            while (i < stop) {
                const char* n = entry_name(i);
                if (strncasecmp(n, path, len) == 0 && n[len] == '\0') {
                    found = (long)i;
                    break;
                }
                i = is_dir(i) ? fst[i].length_or_next : i + 1;
            }
            if (found < 0)
                return -1;
            if (*end == '\0')
                return found;
            if (!is_dir((unsigned long)found))
                return -1;
            dir = found;
        }
        path = *end ? end + 1 : end;
    }
    return dir;
}

int DVDFastOpen(long entrynum, DVDFileInfo* fi)
{
    if (entrynum < 0 || (unsigned long)entrynum >= fst_count || is_dir((unsigned long)entrynum))
        return 0;
    memset(fi, 0, sizeof(*fi));
    fi->startAddr = fst[entrynum].offset_or_parent;
    fi->length = fst[entrynum].length_or_next;
    fi->cb.state = DVD_STATE_END;
    return 1;
}

int DVDOpen(const char* path, DVDFileInfo* fi)
{
    return DVDFastOpen(DVDConvertPathToEntrynum(path), fi);
}

int DVDClose(DVDFileInfo* fi)
{
    (void)fi;
    return 1;
}

long DVDRead(DVDFileInfo* fi, void* addr, long length, long offset, long prio)
{
    (void)prio;
    if (offset < 0 || length < 0 || !read_image(addr, fi->startAddr + (unsigned long)offset, (unsigned long)length))
        return -1;
    return length;
}

/* ---- asynchronous requests ----------------------------------------------
 * The read happens at once; the completion is a DVD "interrupt" delivered by
 * the interrupt thread (os_irq.c) a millisecond later, with interrupts
 * disabled, as on the GameCube. */

extern long long OSGetTime(void);
extern void __port_irq_post(long long when, void (*fn)(void*, long), void* arg, long value);

#define DVD_LATENCY_TICKS (40500000ll / 1000)    /* 1 ms */

static int busy;

static void complete(void* arg, long result)
{
    DVDCommandBlock* cb = arg;
    busy--;
    cb->state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    cb->transferredSize = result < 0 ? 0 : (unsigned long)result;
    if (cb->callback)
        cb->callback(result, cb);
}

static void queue(DVDCommandBlock* cb, long result)
{
    cb->state = DVD_STATE_BUSY;
    busy++;
    __port_irq_post(OSGetTime() + DVD_LATENCY_TICKS, complete, cb, result);
}
int DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, long length, long offset, DVDCBCallback cb, long prio)
{
    fi->cb.command = DVD_COMMAND_READ;
    fi->cb.addr = addr;
    fi->cb.length = (unsigned long)length;
    fi->cb.offset = fi->startAddr + (unsigned long)offset;
    fi->cb.callback = cb;
    fi->callback = cb;
    queue(&fi->cb, DVDRead(fi, addr, length, offset, prio));
    return 1;
}

/* The game calls the 6-argument form under the name DVDReadAsync. */
int DVDReadAsync(DVDFileInfo* fi, void* addr, long length, long offset, DVDCBCallback cb, long prio)
{
    return DVDReadAsyncPrio(fi, addr, length, offset, cb, prio);
}

int DVDSeekAsyncPrio(DVDFileInfo* fi, long offset, DVDCBCallback cb, long prio)
{
    (void)prio;
    fi->cb.command = DVD_COMMAND_SEEK;
    fi->cb.offset = fi->startAddr + (unsigned long)offset;
    fi->cb.callback = cb;
    fi->callback = cb;
    queue(&fi->cb, 0);
    return 1;
}

long DVDSeek(DVDFileInfo* fi, long offset) { (void)fi; (void)offset; return 0; }

long DVDGetCommandBlockStatus(DVDCommandBlock* cb) { return cb->state; }

long DVDGetDriveStatus(void)
{
    return busy ? DVD_STATE_BUSY : DVD_STATE_END;
}

void* DVDGetCurrentDiskID(void) { return __gc_lowmem; }

void DVDSetAutoFatalMessaging(int enable) { (void)enable; }
