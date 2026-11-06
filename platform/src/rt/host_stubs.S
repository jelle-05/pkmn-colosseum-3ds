@ host_stubs.S - the big-endian side's calls into the little-endian host.
@
@ One stub per entry of HOST_FUNCTIONS (port/host.h). It switches data
@ accesses to little-endian, calls the host function through __host_table
@ (filled by the host at startup, in list order), and switches back. The
@ host functions take their arguments in registers only (the rules in
@ host.h), so the stub may push to the stack.

#include "port/host.h"

    .arm
    .text

    .set host_index, 0

.macro host_stub name
    .global \name
    .type \name, %function
    .balign 4
\name:
    push {r4, lr}
    ldr r12, =__host_table
    ldr r12, [r12, #(host_index * 4)]
    setend le
    blx r12
    setend be
    pop {r4, pc}
    .ltorg
    .set host_index, host_index + 1
.endm

#define HOST_STUB(name, ret, params) host_stub name;
HOST_FUNCTIONS(HOST_STUB)
HOST_LIBM(HOST_STUB)

    .bss
    .balign 4
    .global __host_table
__host_table:
    .space host_index * 4
