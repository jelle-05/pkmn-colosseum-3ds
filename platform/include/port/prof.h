/* prof.h - frame time per slot, see platform/src/os/prof.c. */
#ifndef PORT_PROF_H
#define PORT_PROF_H

enum { PROF_VERTICES, PROF_GPU_SUBMIT, PROF_FRAME_END, PROF_RETRACE_WAIT, PROF_IRQ_WAIT, PROF_DVD,
       PROF_IRQ_HANDLERS, PROF_FRAME, PROF_SLOTS };

long long __prof_now(void);
void __prof_add(int slot, long long ticks);
void __prof_frame(void);
void __prof_wait(void* from, long long ticks);   /* the main thread blocked, by call site */

#define PROF_BEGIN(name) long long name = __prof_now()
#define PROF_END(slot, name) __prof_add(slot, __prof_now() - (name))

#endif
