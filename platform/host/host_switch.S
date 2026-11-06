@ host_switch.S - entering the big-endian image from the host.
@
@ void host_be_call(host_entry fn, void* arg): runs fn(arg) with big-endian
@ data accesses, then returns to little-endian. Used for the entry point and
@ every thread the big-endian side creates.

    .arm
    .text
    .global host_be_call
    .type host_be_call, %function
host_be_call:
    push {r4, lr}
    mov r4, r0
    mov r0, r1
    setend be
    blx r4
    setend le
    pop {r4, pc}
