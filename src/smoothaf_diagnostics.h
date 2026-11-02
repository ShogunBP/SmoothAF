#ifndef SMOOTHAF_DIAGNOSTICS_H
#define SMOOTHAF_DIAGNOSTICS_H

#include <stdint.h>
#include <stddef.h>

#define SMOOTHAF_DIAG_CAPACITY 32

struct smoothaf_telemetry
{
    int32_t score;
    uint32_t duration_ms;
    int32_t direction;
    int32_t step;
    int32_t state;
    int32_t error;
    int32_t reason;
};

static inline void smoothaf_telemetry_init(struct smoothaf_telemetry *telemetry)
{
    telemetry->score = 0;
    telemetry->duration_ms = 0;
    telemetry->direction = 0;
    telemetry->step = 0;
    telemetry->state = 0;
    telemetry->error = 0;
    telemetry->reason = 0;
}

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

struct smoothaf_log_entry
{
    smoothaf_log_type_t type;
    uint32_t timestamp_ms;
    int32_t value1;
    int32_t value2;
    int32_t value3;
    int32_t value4;
    int32_t value5;
};

struct smoothaf_log_buffer
{
    struct smoothaf_log_entry entries[SMOOTHAF_DIAG_CAPACITY];
    size_t head;
    size_t count;
    int enabled;
    uint32_t tick;
    size_t dropped;
};

static inline void smoothaf_log_init(struct smoothaf_log_buffer *log)
{
    log->head = 0;
    log->count = 0;
    log->enabled = 0;
    log->tick = 0;
    log->dropped = 0;
}

static inline void smoothaf_log_set_enabled(struct smoothaf_log_buffer *log, int enabled)
{
    log->enabled = enabled ? 1 : 0;
}

static inline int smoothaf_log_is_enabled(const struct smoothaf_log_buffer *log)
{
    return log->enabled;
}

static inline void smoothaf_log_tick(struct smoothaf_log_buffer *log, uint32_t delta_ms)
{
    log->tick += delta_ms;
}

static inline uint32_t smoothaf_log_now(const struct smoothaf_log_buffer *log)
{
    return log->tick;
}

static inline void smoothaf_log_write_extended(struct smoothaf_log_buffer *log,
                                               smoothaf_log_type_t type,
                                               int32_t v1, int32_t v2,
                                               int32_t v3, int32_t v4,
                                               int32_t v5)
{
    struct smoothaf_log_entry *entry;
    if (!log->enabled) return;
    entry = &log->entries[log->head];
    entry->type = type;
    entry->timestamp_ms = log->tick;
    entry->value1 = v1;
    entry->value2 = v2;
    entry->value3 = v3;
    entry->value4 = v4;
    entry->value5 = v5;
    log->head = (log->head + 1) % SMOOTHAF_DIAG_CAPACITY;
    if (log->count < SMOOTHAF_DIAG_CAPACITY) log->count++;
    else log->dropped++;
}

static inline void smoothaf_log_write(struct smoothaf_log_buffer *log,
                                      smoothaf_log_type_t type,
                                      int32_t v1, int32_t v2, int32_t v3)
{
    smoothaf_log_write_extended(log, type, v1, v2, v3, 0, 0);
}

static inline size_t smoothaf_log_snapshot(const struct smoothaf_log_buffer *log,
                                           struct smoothaf_log_entry *out,
                                           size_t max_entries)
{
    size_t n = log->count < max_entries ? log->count : max_entries;
    size_t start;
    size_t i;
    if (!n) return 0;
    start = (log->head + SMOOTHAF_DIAG_CAPACITY - log->count) % SMOOTHAF_DIAG_CAPACITY;
    for (i = 0; i < n; i++) out[i] = log->entries[(start + i) % SMOOTHAF_DIAG_CAPACITY];
    return n;
}

static inline size_t smoothaf_log_read_all(const struct smoothaf_log_buffer *log,
                                           struct smoothaf_log_entry *out,
                                           size_t max_entries)
{
    return smoothaf_log_snapshot(log, out, max_entries);
}

static inline const char *smoothaf_log_type_name(smoothaf_log_type_t type)
{
    switch (type)
    {
        case SMOOTHAF_LOG_MOVE: return "MOVE";
        case SMOOTHAF_LOG_MEASURE: return "MEASURE";
        case SMOOTHAF_LOG_CONTROLLER: return "CONTROL";
        case SMOOTHAF_LOG_ABORT: return "ABORT";
        case SMOOTHAF_LOG_ERROR: return "ERROR";
        case SMOOTHAF_LOG_BENCH_START: return "BENCH+";
        case SMOOTHAF_LOG_BENCH_END: return "BENCH-";
        case SMOOTHAF_LOG_TASK_END: return "TASK-END";
        case SMOOTHAF_LOG_MONITOR: return "MONITOR";
        default: return "NONE";
    }
}

static inline void smoothaf_log_clear(struct smoothaf_log_buffer *log)
{
    log->head = 0;
    log->count = 0;
    log->tick = 0;
    log->dropped = 0;
}

#endif
