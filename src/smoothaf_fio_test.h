#ifndef SMOOTHAF_FIO_TEST_H
#define SMOOTHAF_FIO_TEST_H

#include <stddef.h>
#include <stdint.h>

enum smoothaf_fio_variant
{
    SMOOTHAF_FIO_ROOT_CREATE = 0,
    SMOOTHAF_FIO_ROOT_EDIT = 1,
    SMOOTHAF_FIO_FILE_PATHS = 2,
};

enum smoothaf_fio_phase
{
    SMOOTHAF_FIO_READY = 0,
    SMOOTHAF_FIO_BLOCKED_TASK,
    SMOOTHAF_FIO_NAME_CHECK,
    SMOOTHAF_FIO_NOT_FOUND,
    SMOOTHAF_FIO_COLLISION,
    SMOOTHAF_FIO_SIZE_CHECK,
    SMOOTHAF_FIO_SIZE_MISMATCH,
    SMOOTHAF_FIO_OPEN,
    SMOOTHAF_FIO_SEEK_SET,
    SMOOTHAF_FIO_WRITE,
    SMOOTHAF_FIO_CLOSE,
    SMOOTHAF_FIO_SUCCESS,
    SMOOTHAF_FIO_ERROR,
    SMOOTHAF_FIO_ABI_UNAVAILABLE,
    SMOOTHAF_FIO_PATH_SCAN,
};

struct smoothaf_fio_state
{
    enum smoothaf_fio_variant variant;
    enum smoothaf_fio_phase phase;
    int reserved;
    int published;
    int handle_valid;
    int close_called;
    int collision;
    int file_exists;
    int write_return;
    int open_return;
    int seek_return;
    int size_return;
    size_t file_size;
    size_t bytes_requested;
    size_t bytes_written;
};

struct smoothaf_fio_path_state
{
    enum smoothaf_fio_phase phase;
    int is_file_called;
    int is_file_return;
    int size_called;
    int size_return;
    unsigned int file_size;
    int open_called;
    int open_return;
    uintptr_t open_handle;
    int handle_valid;
    int close_called;
};

static inline void smoothaf_fio_state_init(struct smoothaf_fio_state *state,
                                           enum smoothaf_fio_variant variant)
{
    state->variant = variant;
    state->phase = SMOOTHAF_FIO_READY;
    state->reserved = 0;
    state->published = 0;
    state->handle_valid = 0;
    state->close_called = 0;
    state->collision = 0;
    state->file_exists = 0;
    state->write_return = 0;
    state->open_return = 0;
    state->seek_return = 0;
    state->size_return = 0;
    state->file_size = 0;
    state->bytes_requested = 0;
    state->bytes_written = 0;
}

static inline void smoothaf_fio_path_state_init(struct smoothaf_fio_path_state *state)
{
    state->phase = SMOOTHAF_FIO_READY;
    state->is_file_called = 0;
    state->is_file_return = 0;
    state->size_called = 0;
    state->size_return = 0;
    state->file_size = 0;
    state->open_called = 0;
    state->open_return = 0;
    state->open_handle = (uintptr_t)0;
    state->handle_valid = 0;
    state->close_called = 0;
}

static inline int smoothaf_fio_reserve(struct smoothaf_fio_state *state,
                                       int task_running,
                                       int task_exited,
                                       int monitor_active)
{
    if (state->reserved || task_running || !task_exited || monitor_active)
        return 0;
    state->reserved = 1;
    return 1;
}

static inline void smoothaf_fio_release(struct smoothaf_fio_state *state)
{ state->reserved = 0; }

static inline void smoothaf_fio_blocked(struct smoothaf_fio_state *state)
{ state->phase = SMOOTHAF_FIO_BLOCKED_TASK; }

static inline void smoothaf_fio_name_check(struct smoothaf_fio_state *state)
{ state->phase = SMOOTHAF_FIO_NAME_CHECK; }

static inline void smoothaf_fio_not_found(struct smoothaf_fio_state *state)
{ state->phase = SMOOTHAF_FIO_NOT_FOUND; }

static inline void smoothaf_fio_collision(struct smoothaf_fio_state *state)
{
    state->collision = 1;
    state->phase = SMOOTHAF_FIO_COLLISION;
}

static inline void smoothaf_fio_size_check(struct smoothaf_fio_state *state,
                                           size_t file_size)
{
    state->file_size = file_size;
    state->phase = SMOOTHAF_FIO_SIZE_CHECK;
}

static inline void smoothaf_fio_size_mismatch(struct smoothaf_fio_state *state)
{ state->phase = SMOOTHAF_FIO_SIZE_MISMATCH; }

static inline void smoothaf_fio_opened(struct smoothaf_fio_state *state,
                                       int open_return)
{
    state->open_return = open_return;
    state->handle_valid = open_return != 0;
    state->phase = SMOOTHAF_FIO_OPEN;
}

static inline void smoothaf_fio_seeked(struct smoothaf_fio_state *state,
                                       int seek_return)
{
    state->seek_return = seek_return;
    state->phase = SMOOTHAF_FIO_SEEK_SET;
}

static inline void smoothaf_fio_write_result(struct smoothaf_fio_state *state,
                                             int write_return,
                                             size_t requested)
{
    state->write_return = write_return;
    state->bytes_requested = requested;
    state->bytes_written = write_return > 0 ? (size_t)write_return : 0;
    state->phase = SMOOTHAF_FIO_WRITE;
}

static inline void smoothaf_fio_closed(struct smoothaf_fio_state *state)
{
    state->close_called = 1;
    state->phase = SMOOTHAF_FIO_CLOSE;
}

static inline void smoothaf_fio_publish(struct smoothaf_fio_state *state,
                                        int success)
{
    if (!state->close_called || state->published)
        return;
    state->published = 1;
    state->phase = success ? SMOOTHAF_FIO_SUCCESS : SMOOTHAF_FIO_ERROR;
}

static inline void smoothaf_fio_path_result(struct smoothaf_fio_path_state *state,
                                            int is_file_called,
                                            int is_file_return,
                                            int size_called,
                                            int size_return,
                                            unsigned int file_size,
                                            int open_called,
                                            int open_return,
                                            uintptr_t open_handle,
                                            int close_called)
{
    state->is_file_called = is_file_called;
    state->is_file_return = is_file_return;
    state->size_called = size_called;
    state->size_return = size_return;
    state->file_size = file_size;
    state->open_called = open_called;
    state->open_return = open_return;
    state->open_handle = open_handle;
    state->handle_valid = open_called && open_return != 0 && open_handle != 0;
    state->close_called = close_called && state->handle_valid;
    state->phase = SMOOTHAF_FIO_PATH_SCAN;
}

static inline void smoothaf_fio_error(struct smoothaf_fio_state *state)
{ state->phase = SMOOTHAF_FIO_ERROR; }

#endif
