#ifndef SMOOTHAF_TASK_LIFECYCLE_H
#define SMOOTHAF_TASK_LIFECYCLE_H

/* Publication helpers shared by the production task starter and host tests.
 * A caller must invoke the reserve helper only after its semaphore ownership
 * and start policy checks have succeeded.  Before that point, a failed start
 * must leave the prior lifecycle state untouched. */
static inline int smoothaf_task_reserve(volatile int *running,
                                         volatile int *exited)
{
    if (!running || !exited || *running) return 0;
    *running = 1;
    *exited = 0;
    return 1;
}

static inline void smoothaf_task_create_failed(volatile int *running,
                                                volatile int *exited)
{
    if (!running || !exited) return;
    *running = 0;
    *exited = 1;
}

#endif
