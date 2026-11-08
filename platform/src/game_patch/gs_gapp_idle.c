/*
 * gs_gapp_idle.c - the game's idle function, without the busy lock.
 *
 * gappBackgroundCallback (src/game/gs_gapp.c) is what the game installs with
 * OSSetIdleFunction. It loops forever: run the tasks in the background
 * state, then turn interrupts off to merge tasks created meanwhile into the
 * sorted list. On the GameCube it only runs when no thread can.
 *
 * On the 3DS it runs on the lowest-priority thread, and every
 * OSDisableInterrupts takes the port's interrupt lock, which raises the
 * holder above the game threads (os/os_irq.c). With no background work the
 * loop did that about 140,000 times a second, two priority syscalls each,
 * preempting the main thread every time.
 *
 * This version does the same work, but takes the lock only when tasks are
 * waiting to be merged, and sleeps a millisecond when no background task
 * ran. Same task order and the same list updates as the original.
 */
#include "port/host.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef int s32;

typedef void (*GSgappCallback)(u32 taskId, void* param);

#define GAPP_STATE_BACKGROUND 2

typedef struct GSgappTask {
    struct GSgappTask* prev;
    struct GSgappTask* next;
    s32 state;
    u8 priority;
    u8 blocked;
    u8 _pad[2];
    void* param;
    GSgappCallback callback;
} GSgappTask;

typedef char GSgappTaskSize[sizeof(GSgappTask) == 0x18 ? 1 : -1];

extern GSgappTask* lbl_8047AC7C;    /* task array */
extern GSgappTask* lbl_8047AC94;    /* task being dispatched */
extern GSgappTask* lbl_8047AC98;    /* head of the priority-sorted list */
extern GSgappTask* lbl_8047AC9C;    /* tasks created since, to merge */

extern int OSDisableInterrupts(void);
extern int OSRestoreInterrupts(int level);

/* As gs_gapp.c's gappInsertTask. */
static void insert_task(GSgappTask* task)
{
    GSgappTask* curr = lbl_8047AC98;
    while (curr->next != 0 && curr->priority < task->priority)
        curr = curr->next;
    if (curr->next == 0 && curr->priority < task->priority) {
        task->prev = curr;
        task->next = 0;
        curr->next = task;
    } else {
        if (curr->prev != 0)
            curr->prev->next = task;
        task->prev = curr->prev;
        task->next = curr;
        curr->prev = task;
        if (lbl_8047AC98 == curr)
            lbl_8047AC98 = task;
    }
}

void gappBackgroundCallback(void)
{
    for (;;) {
        int ran = 0;
        GSgappTask* task = *(GSgappTask* volatile*)&lbl_8047AC98;
        while (task != 0) {
            GSgappTask* next = task->next;
            if (task->state == GAPP_STATE_BACKGROUND && !task->blocked) {
                lbl_8047AC94 = task;
                task->callback(((u32)task - (u32)lbl_8047AC7C) / sizeof(GSgappTask) + 1, task->param);
                ran = 1;
            }
            task = next;
        }
        lbl_8047AC94 = 0;

        if (*(GSgappTask* volatile*)&lbl_8047AC9C != 0) {
            int level = OSDisableInterrupts();
            task = lbl_8047AC9C;
            while (task != 0) {
                GSgappTask* next = task->next;
                insert_task(task);
                task = next;
            }
            lbl_8047AC9C = 0;
            OSRestoreInterrupts(level);
        } else if (!ran) {
            host_sleep_us(1000);
        }
    }
}
