/*
 * gc_lowmem.c - stand-in for the first 16 KB of GameCube RAM.
 *
 * See platform/include/port/gc_mem.h. The OS globals are filled in with the
 * values a retail GameCube's boot ROM leaves there for this game, in native
 * byte order, because game code reads them as plain u32s.
 */
#include <stdio.h>
#include <string.h>

#include "port/gc_mem.h"

unsigned char __gc_lowmem[GC_LOWMEM_SIZE] __attribute__((aligned(32)));

static void put32(unsigned off, unsigned long v)
{
    memcpy(__gc_lowmem + off, &v, 4);
}

/* Runs before main(). */
__attribute__((constructor)) static void gc_lowmem_init(void)
{
    memset(__gc_lowmem, 0, sizeof(__gc_lowmem));
    memcpy(__gc_lowmem + 0x00, "GC6E01", 6);     /* disc ID */
    put32(0x20, 0x0D15EA5E);                    /* boot magic */
    put32(0x24, 1);                             /* boot version */
    put32(0x28, 0x01800000);                    /* physical memory size, 24 MB */
    put32(0x2C, 0x00000003);                    /* console type: retail */
    put32(0xF0, 0x01800000);                    /* simulated memory size */
    put32(0xF8, 162000000);                     /* bus clock */
    put32(0xFC, 486000000);                     /* CPU clock */
}

void __port_bad_address(unsigned long addr)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "port: fixed GameCube address 0x%08lX", addr);
    (void)n;
    extern void __port_fatal(const char*);
    snprintf(line, sizeof(line), "bad address 0x%08lX", addr);
    __port_fatal(line);
    for (;;) {
    }
}
