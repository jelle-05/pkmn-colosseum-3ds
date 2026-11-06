@ co_switch.S - the coroutine switch behind the game's GSthread tasks.
@
@ Saves the callee-saved state of the running coroutine (r4-r11, lr and the
@ VFP registers d8-d15, the registers AAPCS requires a callee to preserve)
@ on its own stack, stores that stack pointer, and resumes another
@ coroutine from its stack. A new coroutine's stack is prepared so that the
@ first switch "returns" into __port_co_start with its record in r4.

    .arm
    .syntax unified
    .fpu vfp
    .text

@ void __port_co_switch(void** save_sp, void* load_sp)
    .global __port_co_switch
    .type __port_co_switch, %function
__port_co_switch:
    push    {r4-r11, lr}
    vpush   {d8-d15}
    str     sp, [r0]
    mov     sp, r1
    vpop    {d8-d15}
    pop     {r4-r11, pc}
    .size __port_co_switch, . - __port_co_switch

@ First entry of a new coroutine: r4 holds its record.
    .global __port_co_start
    .type __port_co_start, %function
__port_co_start:
    mov     r0, r4
    bl      __port_co_main
1:  b       1b
    .size __port_co_start, . - __port_co_start
