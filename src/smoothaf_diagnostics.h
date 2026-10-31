/*
 * SmoothAF Diagnostics - Fixed-Size Circular Logging
 *
 * Host-testable, fixed-size circular buffers with wrap-around indexing.
 * No dynamic allocation. C99 compatible. OFF/ON control.
 * Capacity 32 for constrained targets (DIGIC IV).
 */

#ifndef SMOOTHAF_DIAGNOSTICS_H
#define SMOOTHAF_DIAGNOSTICS_H

#include <stdint.h>
#include <stddef.h>

/* Configuration - compile-time constants */
#define SMOOTHAF_DIAG_CAPACITY      32

/* Log entry types */
typedef enum
{
    SMOOTHAF_LOG_NONE = 0,
    SMOOTHAF_LOG_MOVE,
    SMOOTHAF_LOG_MEASURE,
    SMOOTHAF_LOG_CONTROLLER,
    SMOOTHAF_LOG_ABORT,
    SMOOTHAF_LOG_ERROR,
    SMOOTHAF_LOG_BENCH_START,
    SMOOTHAF_LOG_BENCH_END,
    SMOOTHAF_LOG_TASK_END,
    SMOOTHAF_LOG_MONITOR,
} smoothaf_log_type_t;

/* Single log entry - fixed size, no pointers.  value1..value5 are the
 * stable event payload; the first three remain compatible with old readers. */
struct smoothaf_log_entry
{
    smoothaf_log_type_t type;
    uint32_t timestamp_ms;   /* Monotonic module tick counter */
    int32_t value1;          /* Event-specific primary value */
    int32_t value2;          /* Event-specific secondary value */
    int32_t value3;          /* Event-specific tertiary value */
    int32_t value4;          /* Optional position/direction/return value */
    int32_t value5;          /* Optional phase/reason/error value */
};

/* Controller payload: action, best score, best position, current position,
 * direction.  Task-end payload: result, best score, current position, command
 * count, reversal count. */

/* Circular log buffer - fixed capacity, wrap-around */
struct smoothaf_log_buffer
{
    struct smoothaf_log_entry entries[SMOOTHAF_DIAG_CAPACITY];
    size_t head;             /* Next write position (0..CAPACITY-1) */
    size_t count;            /* Number of valid entries (0..CAPACITY) */
    int enabled;             /* 0 = OFF, 1 = ON */
    uint32_t tick;           /* Simulated time counter */
    size_t dropped;          /* Entries overwritten due to wrap-around */
};

/* Initialize log buffer */
static inline void smoothaf_log_init(struct smoothaf_log_buffer *log)
{
    log->head = 0;
    log->count = 0;
    log->enabled = 0;
    log->tick = 0;
    log->dropped = 0;
}

/* Enable/disable logging */
static inline void smoothaf_log_set_enabled(struct smoothaf_log_buffer *log, int enabled)
{
    log->enabled = enabled ? 1 : 0;
}

static inline int smoothaf_log_is_enabled(const struct smoothaf_log_buffer *log)
{
    return log->enabled;
}

/* Advance simulated time */
static inline void smoothaf_log_tick(struct smoothaf_log_buffer *log, uint32_t delta_ms)
{
    log->tick += delta_ms;
}

static inline uint32_t smoothaf_log_now(const struct smoothaf_log_buffer *log)
{
    return log->tick;
}

/* Write a log entry (no-op if disabled) */
static inline void smoothaf_log_write(struct smoothaf_log_buffer *log,
                                      smoothaf_log_type_t type,
                                      int32_t v1, int32_t v2, int32_t v3)
{
    if (!log->enabled) return;

    struct smoothaf_log_entry *e = &log->entries[log->head];
    e->type = type;
    e->timestamp_ms = log->tick;
    e->value1 = v1;
    e->value2 = v2;
    e->value3 = v3;
    e->value4 = 0;
    e->value5 = 0;

    log->head = (log->head + 1) % SMOOTHAF_DIAG_CAPACITY;
    if (log->count < SMOOTHAF_DIAG_CAPACITY)
    {
        log->count++;
    }
    else
    {
        log->dropped++;
    }
}

/* Write an event with the complete fixed payload. */
static inline void smoothaf_log_write_extended(struct smoothaf_log_buffer *log,
                                               smoothaf_log_type_t type,
                                               int32_t v1, int32_t v2,
                                               int32_t v3, int32_t v4,
                                               int32_t v5)
{
    if (!log->enabled) return;

    struct smoothaf_log_entry *e = &log->entries[log->head];
    e->type = type;
    e->timestamp_ms = log->tick;
    e->value1 = v1;
    e->value2 = v2;
    e->value3 = v3;
    e->value4 = v4;
    e->value5 = v5;

    log->head = (log->head + 1) % SMOOTHAF_DIAG_CAPACITY;
    if (log->count < SMOOTHAF_DIAG_CAPACITY)
        log->count++;
    else
        log->dropped++;
}

/* Convenience logging macros for host tests (compile to nothing if disabled) */
#define SMOOTHAF_LOG_WRITE_MOVE(log, dir, pos) \
    smoothaf_log_write_extended(log, SMOOTHAF_LOG_MOVE, (dir), (pos), 0, 0, 0)

#define SMOOTHAF_LOG_WRITE_MEASURE(log, score, mean, pos) \
    smoothaf_log_write_extended(log, SMOOTHAF_LOG_MEASURE, (score), (mean), (pos), 0, 0)

#define SMOOTHAF_LOG_WRITE_CONTROLLER(log, action, best, best_pos, pos, dir) \
    smoothaf_log_write_extended(log, SMOOTHAF_LOG_CONTROLLER, (action), (best), \
                                (best_pos), (pos), (dir))

#define SMOOTHAF_LOG_WRITE_TASK_END(log, result, best, pos, commands, reversals) \
    smoothaf_log_write_extended(log, SMOOTHAF_LOG_TASK_END, (result), (best), \
                                (pos), (commands), (reversals))

#define SMOOTHAF_LOG_WRITE_ABORT(log, reason) \
    smoothaf_log_write(log, SMOOTHAF_LOG_ABORT, (reason), 0, 0)

#define SMOOTHAF_LOG_WRITE_ERROR(log, code) \
    smoothaf_log_write(log, SMOOTHAF_LOG_ERROR, (code), 0, 0)

#define SMOOTHAF_LOG_WRITE_BENCH_START(log, iter) \
    smoothaf_log_write(log, SMOOTHAF_LOG_BENCH_START, (iter), 0, 0)

#define SMOOTHAF_LOG_WRITE_BENCH_END(log, iter, result) \
    smoothaf_log_write(log, SMOOTHAF_LOG_BENCH_END, (iter), (result), 0)

/* Read entries in chronological order (oldest first) */
static inline size_t smoothaf_log_read_all(const struct smoothaf_log_buffer *log,
                                           struct smoothaf_log_entry *out,
                                           size_t max_entries)
{
    size_t n = log->count < max_entries ? log->count : max_entries;
    size_t start;
    size_t i;

    if (n == 0) return 0;

    start = (log->head + SMOOTHAF_DIAG_CAPACITY - log->count) % SMOOTHAF_DIAG_CAPACITY;
    for (i = 0; i < n; i++)
    {
        out[i] = log->entries[(start + i) % SMOOTHAF_DIAG_CAPACITY];
    }
    return n;
}

/* Copy one bounded, chronological snapshot for read-only viewers. */
static inline size_t smoothaf_log_snapshot(const struct smoothaf_log_buffer *log,
                                           struct smoothaf_log_entry *out,
                                           size_t max_entries)
{
    return smoothaf_log_read_all(log, out, max_entries);
}

static inline const char *smoothaf_log_type_name(smoothaf_log_type_t type)
{
    switch (type)
    {
        case SMOOTHAF_LOG_MOVE:        return "MOVE";
        case SMOOTHAF_LOG_MEASURE:     return "MEASURE";
        case SMOOTHAF_LOG_CONTROLLER:  return "CONTROL";
        case SMOOTHAF_LOG_ABORT:       return "ABORT";
        case SMOOTHAF_LOG_ERROR:       return "ERROR";
        case SMOOTHAF_LOG_BENCH_START: return "BENCH+";
        case SMOOTHAF_LOG_BENCH_END:   return "BENCH-";
        case SMOOTHAF_LOG_TASK_END:    return "TASK-END";
        case SMOOTHAF_LOG_MONITOR:     return "MONITOR";
        default:                       return "NONE";
    }
}

/* Clear buffer */
static inline void smoothaf_log_clear(struct smoothaf_log_buffer *log)
{
    log->head = 0;
    log->count = 0;
    log->tick = 0;
    log->dropped = 0;
}

#endif /* SMOOTHAF_DIAGNOSTICS_H */