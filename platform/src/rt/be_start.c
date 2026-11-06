/*
 * be_start.c - start of the big-endian image, and the C library glue its
 * newlib (the big-endian multilib) needs.
 *
 * The host's main() fills the host table and calls __be_start in
 * big-endian mode. Here the image's constructors run (the platform's
 * arena, time and thread setup among them), then the game's main().
 */
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/stat.h>

#include "port/host.h"

extern int main(void);

typedef void (*init_fn)(void);
extern init_fn __be_init_array_start[], __be_init_array_end[];

void __be_start(void* unused)
{
    (void)unused;
    for (init_fn* f = __be_init_array_start; f < __be_init_array_end; f++)
        (*f)();
    host_exit(main());
}

/* ---- newlib ---------------------------------------------------------------------
 * Only its string and formatting functions are used; memory comes from the
 * host's heap. Formatting floats allocates (dtoa), hence the _r versions. */

void* _malloc_r(struct _reent* r, size_t n) { (void)r; return host_malloc(n); }
void _free_r(struct _reent* r, void* p) { (void)r; host_free(p); }
void* _calloc_r(struct _reent* r, size_t a, size_t b)
{
    (void)r;
    size_t n = a * b;
    void* p = host_malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void* _realloc_r(struct _reent* r, void* p, size_t n) { (void)r; return host_realloc(p, n); }

void* malloc(size_t n) { return host_malloc(n); }
void free(void* p) { host_free(p); }
void* calloc(size_t a, size_t b) { return _calloc_r(0, a, b); }
void* realloc(void* p, size_t n) { return _realloc_r(0, p, n); }
void* memalign(size_t a, size_t n) { return host_memalign(a, n); }

/* devkitARM's newlib is built for devkitPro's system layer (libsysbase):
 * these are the parts its formatting and stdio code reference. Locks are
 * no-ops: only formatting into buffers is used, never shared FILEs. */
struct _reent* __getreent(void) { return _impure_ptr; }
void __libc_lock_acquire_recursive(_LOCK_RECURSIVE_T* lock) { (void)lock; }
void __libc_lock_release_recursive(_LOCK_RECURSIVE_T* lock) { (void)lock; }
void __libc_lock_acquire(_LOCK_T* lock) { (void)lock; }
void __libc_lock_release(_LOCK_T* lock) { (void)lock; }

int _write_r(struct _reent* r, int fd, const void* buf, size_t n)
{
    (void)r; (void)fd;
    host_debug_string(buf, n);
    return (int)n;
}
int _read_r(struct _reent* r, int fd, void* buf, size_t n) { (void)r; (void)fd; (void)buf; (void)n; return 0; }
int _close_r(struct _reent* r, int fd) { (void)r; (void)fd; return -1; }
_off_t _lseek_r(struct _reent* r, int fd, _off_t off, int whence)
{
    (void)r; (void)fd; (void)off; (void)whence;
    return -1;
}
int _fstat_r(struct _reent* r, int fd, struct stat* st) { (void)r; (void)fd; (void)st; return -1; }
int _isatty_r(struct _reent* r, int fd) { (void)r; (void)fd; return 0; }

/* System calls newlib may reference; nothing here does I/O through it. */
void* _sbrk(ptrdiff_t n) { (void)n; errno = ENOMEM; return (void*)-1; }
void _exit(int code) { host_exit(code); for (;;) {} }
int _write(int fd, const void* buf, size_t n) { (void)fd; host_debug_string(buf, n); return (int)n; }
int _read(int fd, void* buf, size_t n) { (void)fd; (void)buf; (void)n; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int _fstat(int fd, void* st) { (void)fd; (void)st; return -1; }
int _isatty(int fd) { (void)fd; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
void abort(void) { host_break(0); for (;;) {} }
