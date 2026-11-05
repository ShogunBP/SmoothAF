#ifndef SMOOTHAF_RUNTIME_H
#define SMOOTHAF_RUNTIME_H

#include <stdint.h>

/* Small resource/lifecycle seam used by host tests while the ARM adapter
 * remains unchanged. The control semaphore is mandatory. The FIO consumer task
 * is optional and must never turn a usable monitor into an unusable one. */

enum smoothaf_runtime_error {
    SMOOTHAF_RUNTIME_OK = 0,
    SMOOTHAF_RUNTIME_SEMAPHORE_CREATE = 1,
    SMOOTHAF_RUNTIME_SEMAPHORE_ACQUIRE = 2,
    SMOOTHAF_RUNTIME_SEMAPHORE_RELEASE = 3,
    SMOOTHAF_RUNTIME_FIO_TASK = 4
};

typedef void *(*smoothaf_runtime_create_semaphore_fn)(const char *name,
                                                       int initial_value);
typedef int (*smoothaf_runtime_semaphore_fn)(void *semaphore, int timeout);
typedef int (*smoothaf_runtime_give_semaphore_fn)(void *semaphore);
typedef void *(*smoothaf_runtime_create_task_fn)(const char *name,
                                                  uint32_t priority,
                                                  uint32_t stack_size,
                                                  void *entry,
                                                  void *arg);
typedef int (*smoothaf_runtime_task_valid_fn)(void *task);
typedef void (*smoothaf_runtime_stop_task_fn)(void *task);

struct smoothaf_runtime_ops {
    smoothaf_runtime_create_semaphore_fn create_semaphore;
    smoothaf_runtime_semaphore_fn take_semaphore;
    smoothaf_runtime_give_semaphore_fn give_semaphore;
    smoothaf_runtime_create_task_fn create_task;
    smoothaf_runtime_task_valid_fn task_valid;
    smoothaf_runtime_stop_task_fn stop_task;
};

struct smoothaf_runtime_state {
    void *control_semaphore;
    void *fio_task;
    int ready;
    int fio_requested;
    int fio_enabled;
    int fio_task_created;
    int fio_task_running;
    int fio_task_exited; /* 1 after failed creation or consumer exit */
    int monitor_started;
    int stop_requested;
    int panic;
    int lock_held;
    int first_error;
    int last_error;
    int error;
};

static inline void smoothaf_runtime_note_error(struct smoothaf_runtime_state *state,
                                               int error)
{
    if (!state || error == SMOOTHAF_RUNTIME_OK) return;
    if (state->first_error == SMOOTHAF_RUNTIME_OK)
        state->first_error = error;
    state->last_error = error;
}

static inline void smoothaf_runtime_reset(struct smoothaf_runtime_state *state)
{
    if (!state) return;
    state->control_semaphore = 0;
    state->fio_task = 0;
    state->ready = 0;
    state->fio_requested = 0;
    state->fio_enabled = 0;
    state->fio_task_created = 0;
    state->fio_task_running = 0;
    state->fio_task_exited = 1;
    state->monitor_started = 0;
    state->stop_requested = 0;
    state->panic = 0;
    state->lock_held = 0;
    state->first_error = SMOOTHAF_RUNTIME_OK;
    state->last_error = SMOOTHAF_RUNTIME_OK;
    state->error = SMOOTHAF_RUNTIME_OK;
}

/* Validate the object immediately.  A non-null but unusable handle must not
 * survive initialization and later masquerade as a monitor-start failure. */
static inline int smoothaf_runtime_init(struct smoothaf_runtime_state *state,
                                         const struct smoothaf_runtime_ops *ops,
                                         int fio_requested)
{
    int result;
    if (!state || !ops || !ops->create_semaphore ||
        !ops->take_semaphore || !ops->give_semaphore) return 0;

    smoothaf_runtime_reset(state);
    state->control_semaphore = ops->create_semaphore("smoothaf_sem", 1);
    if (!state->control_semaphore) {
        state->error = SMOOTHAF_RUNTIME_SEMAPHORE_CREATE;
        smoothaf_runtime_note_error(state, state->error);
        return 0;
    }

    result = ops->take_semaphore(state->control_semaphore, 0);
    if (result != 0) {
        state->error = SMOOTHAF_RUNTIME_SEMAPHORE_ACQUIRE;
        smoothaf_runtime_note_error(state, state->error);
        state->control_semaphore = 0;
        return 0;
    }
    state->lock_held = 1;
    result = ops->give_semaphore(state->control_semaphore);
    if (result != 0) {
        state->error = SMOOTHAF_RUNTIME_SEMAPHORE_RELEASE;
        smoothaf_runtime_note_error(state, state->error);
        state->control_semaphore = 0;
        state->lock_held = 0;
        return 0;
    }
    state->lock_held = 0;

    state->ready = 1;
    state->fio_requested = fio_requested ? 1 : 0;
    state->fio_enabled = 0;

    /* FIO is an optional diagnostic sink.  Its task may fail independently. */
    if (state->fio_requested && ops->create_task) {
        state->fio_task = ops->create_task("smoothaf_fio", 0x1d, 0x1000, 0, 0);
        if (state->fio_task && (!ops->task_valid ||
                                ops->task_valid(state->fio_task))) {
            state->fio_task_created = 1;
            state->fio_task_running = 1;
            state->fio_task_exited = 0;
            state->fio_enabled = 1;
        } else {
            state->fio_task = 0;
            state->fio_task_exited = 1;
            state->error = SMOOTHAF_RUNTIME_FIO_TASK;
            smoothaf_runtime_note_error(state, state->error);
        }
    }
    return 1;
}

static inline int smoothaf_runtime_start_monitor(struct smoothaf_runtime_state *state)
{
    if (!state || !state->ready || state->stop_requested || state->panic)
        return 0;
    state->monitor_started = 1;
    return 1;
}

static inline void smoothaf_runtime_stop(struct smoothaf_runtime_state *state)
{
    if (!state) return;
    state->stop_requested = 1;
    state->monitor_started = 0;
}

static inline void smoothaf_runtime_panic(struct smoothaf_runtime_state *state)
{
    if (!state) return;
    state->panic = 1;
    state->stop_requested = 1;
    state->monitor_started = 0;
}

static inline int smoothaf_runtime_can_move(const struct smoothaf_runtime_state *state)
{
    return state && state->ready && state->monitor_started &&
           !state->stop_requested && !state->panic;
}

static inline void smoothaf_runtime_shutdown(struct smoothaf_runtime_state *state,
                                             const struct smoothaf_runtime_ops *ops)
{
    if (!state) return;
    smoothaf_runtime_stop(state);
    if (state->fio_task_created && state->fio_task && ops && ops->stop_task)
        ops->stop_task(state->fio_task);
    state->fio_task_running = 0;
    state->fio_task_exited = 1;
    state->fio_task_created = 0;
    state->fio_task = 0;
    state->fio_enabled = 0;
    state->ready = 0;
}

#endif
