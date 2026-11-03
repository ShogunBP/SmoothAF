#ifndef SMOOTHAF_DIAGNOSTICS_H
#define SMOOTHAF_DIAGNOSTICS_H

#include <stdint.h>
#include <stddef.h>

#define SMOOTHAF_DIAG_CAPACITY 32
#define SMOOTHAF_EPISODE_EVENT_CAPACITY 32
#define SMOOTHAF_EPISODE_SUMMARY_CAPACITY 4

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
    SMOOTHAF_LOG_RECORDING,
    SMOOTHAF_LOG_EPISODE,
} smoothaf_log_type_t;

typedef enum
{
    SMOOTHAF_EPISODE_EVENT_NONE = 0,
    SMOOTHAF_EPISODE_EVENT_ARM = 1,
    SMOOTHAF_EPISODE_EVENT_BLUR = 2,
    SMOOTHAF_EPISODE_EVENT_DECISION = 3,
    SMOOTHAF_EPISODE_EVENT_LENS_CALL = 4,
    SMOOTHAF_EPISODE_EVENT_LENS_RETURN = 5,
    SMOOTHAF_EPISODE_EVENT_SETTLE = 6,
    SMOOTHAF_EPISODE_EVENT_POST_MOVE = 7,
    SMOOTHAF_EPISODE_EVENT_REVERSAL = 8,
    SMOOTHAF_EPISODE_EVENT_GATE = 9,
    SMOOTHAF_EPISODE_EVENT_COMPLETE = 10,
    SMOOTHAF_EPISODE_EVENT_ACQUIRE = 11,
} smoothaf_episode_event_type_t;

typedef enum
{
    SMOOTHAF_EPISODE_RESULT_NONE = 0,
    SMOOTHAF_EPISODE_RESULT_IMPROVED = 1,
    SMOOTHAF_EPISODE_RESULT_WORSE = 2,
    SMOOTHAF_EPISODE_RESULT_UNCERTAIN = 3,
    SMOOTHAF_EPISODE_RESULT_CONVERGED = 4,
    SMOOTHAF_EPISODE_RESULT_TIMEOUT = 5,
    SMOOTHAF_EPISODE_RESULT_BLOCKED = 6,
    SMOOTHAF_EPISODE_RESULT_LENS_FAILED = 7,
    SMOOTHAF_EPISODE_RESULT_NO_IMPROVEMENT = 8,
    SMOOTHAF_EPISODE_RESULT_CANCELLED = 9,
    SMOOTHAF_EPISODE_RESULT_NO_MOVEMENT = 10,
    SMOOTHAF_EPISODE_RESULT_SEARCH_LIMIT = 11,
    SMOOTHAF_EPISODE_RESULT_DRY_RUN = 12,
} smoothaf_episode_result_t;

struct smoothaf_episode_event
{
    uint32_t timestamp_ms;
    uint32_t episode_id;
    uint16_t type;
    int16_t result;
    int16_t relation;
    int16_t reserved;
    int32_t score;
    int32_t best_score;
    int32_t confidence;
    int32_t direction;
    int32_t step;
    int32_t position;
    int32_t best_position;
    int32_t lens_return;
    uint32_t duration_ms;
};

struct smoothaf_episode_summary
{
    uint32_t episode_id;
    uint32_t started_ms;
    uint32_t ended_ms;
    uint32_t image_ms;
    uint32_t wait_ms;
    uint32_t movement_ms;
    uint32_t stabilization_ms;
    uint32_t commands;
    uint32_t reversals;
    int32_t initial_score;
    int32_t best_score;
    int32_t final_score;
    int32_t best_position;
    int32_t final_position;
    int16_t result;
    int16_t lens_calls;
    int16_t mode;
    int16_t block_reason;
    int16_t cancel_reason;
    int16_t lens_return;
    int16_t post_move_valid;
    int16_t confidence;
    int16_t complete;
};

struct smoothaf_episode_log
{
    struct smoothaf_episode_event events[SMOOTHAF_EPISODE_EVENT_CAPACITY];
    struct smoothaf_episode_summary summaries[SMOOTHAF_EPISODE_SUMMARY_CAPACITY];
    size_t event_head;
    size_t event_count;
    size_t event_dropped;
    size_t summary_head;
    size_t summary_count;
};

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

static inline uint32_t smoothaf_diag_elapsed(uint32_t start, uint32_t end)
{
    return end - start;
}

static inline void smoothaf_episode_log_init(struct smoothaf_episode_log *log)
{
    log->event_head = 0;
    log->event_count = 0;
    log->event_dropped = 0;
    log->summary_head = 0;
    log->summary_count = 0;
}

static inline void smoothaf_episode_log_clear(struct smoothaf_episode_log *log)
{
    smoothaf_episode_log_init(log);
}

static inline void smoothaf_episode_log_event(
    struct smoothaf_episode_log *log, uint32_t timestamp_ms,
    uint32_t episode_id, smoothaf_episode_event_type_t type, int result,
    int score, int best_score, int confidence, int direction, int step,
    int position, int best_position, int lens_return,
    uint32_t duration_ms)
{
    struct smoothaf_episode_event *event = &log->events[log->event_head];
    event->timestamp_ms = timestamp_ms;
    event->episode_id = episode_id;
    event->type = (uint16_t)type;
    event->result = (int16_t)result;
    event->relation = 0;
    event->reserved = 0;
    event->score = score;
    event->best_score = best_score;
    event->confidence = confidence;
    event->direction = direction;
    event->step = step;
    event->position = position;
    event->best_position = best_position;
    event->lens_return = lens_return;
    event->duration_ms = duration_ms;
    log->event_head = (log->event_head + 1) % SMOOTHAF_EPISODE_EVENT_CAPACITY;
    if (log->event_count < SMOOTHAF_EPISODE_EVENT_CAPACITY)
        log->event_count++;
    else
        log->event_dropped++;
}

static inline void smoothaf_episode_log_summary(
    struct smoothaf_episode_log *log, const struct smoothaf_episode_summary *summary)
{
    log->summaries[log->summary_head] = *summary;
    log->summary_head = (log->summary_head + 1) % SMOOTHAF_EPISODE_SUMMARY_CAPACITY;
    if (log->summary_count < SMOOTHAF_EPISODE_SUMMARY_CAPACITY)
        log->summary_count++;
}

static inline size_t smoothaf_episode_events_snapshot(
    const struct smoothaf_episode_log *log, struct smoothaf_episode_event *out,
    size_t max_events)
{
    size_t n = log->event_count < max_events ? log->event_count : max_events;
    size_t start;
    size_t i;
    if (!n) return 0;
    start = (log->event_head + SMOOTHAF_EPISODE_EVENT_CAPACITY - log->event_count) %
            SMOOTHAF_EPISODE_EVENT_CAPACITY;
    for (i = 0; i < n; i++)
        out[i] = log->events[(start + i) % SMOOTHAF_EPISODE_EVENT_CAPACITY];
    return n;
}

static inline size_t smoothaf_episode_summaries_snapshot(
    const struct smoothaf_episode_log *log, struct smoothaf_episode_summary *out,
    size_t max_summaries)
{
    size_t n = log->summary_count < max_summaries ? log->summary_count : max_summaries;
    size_t start;
    size_t i;
    if (!n) return 0;
    start = (log->summary_head + SMOOTHAF_EPISODE_SUMMARY_CAPACITY - log->summary_count) %
            SMOOTHAF_EPISODE_SUMMARY_CAPACITY;
    for (i = 0; i < n; i++)
        out[i] = log->summaries[(start + i) % SMOOTHAF_EPISODE_SUMMARY_CAPACITY];
    return n;
}

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

/* count + dropped is monotonic even after the ring buffer is full. */
static inline size_t smoothaf_log_total(const struct smoothaf_log_buffer *log)
{
    return log->count + log->dropped;
}

/* Follow the newest record while leaving a manually selected page alone. */
static inline size_t smoothaf_diag_page_after_refresh(size_t count, size_t total,
                                                      size_t page, size_t *seen_total)
{
    if (total > *seen_total)
    {
        *seen_total = total;
        return count ? count - 1 : 0;
    }
    if (!count) return 0;
    return page < count ? page : count - 1;
}

static inline const char *smoothaf_episode_event_name(int type)
{
    switch (type)
    {
        case SMOOTHAF_EPISODE_EVENT_ARM: return "ARM";
        case SMOOTHAF_EPISODE_EVENT_BLUR: return "BLUR";
        case SMOOTHAF_EPISODE_EVENT_DECISION: return "DECISION";
        case SMOOTHAF_EPISODE_EVENT_LENS_CALL: return "LENS_CALL";
        case SMOOTHAF_EPISODE_EVENT_LENS_RETURN: return "LENS_RETURN";
        case SMOOTHAF_EPISODE_EVENT_SETTLE: return "SETTLE";
        case SMOOTHAF_EPISODE_EVENT_POST_MOVE: return "POST_MOVE";
        case SMOOTHAF_EPISODE_EVENT_REVERSAL: return "REVERSAL";
        case SMOOTHAF_EPISODE_EVENT_GATE: return "GATE";
        case SMOOTHAF_EPISODE_EVENT_COMPLETE: return "COMPLETE";
        case SMOOTHAF_EPISODE_EVENT_ACQUIRE: return "ACQUIRE";
        default: return "NONE";
    }
}

static inline const char *smoothaf_episode_result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_EPISODE_RESULT_IMPROVED: return "IMPROVED";
        case SMOOTHAF_EPISODE_RESULT_WORSE: return "WORSE";
        case SMOOTHAF_EPISODE_RESULT_UNCERTAIN: return "UNCERTAIN";
        case SMOOTHAF_EPISODE_RESULT_CONVERGED: return "CONVERGED";
        case SMOOTHAF_EPISODE_RESULT_TIMEOUT: return "TIMEOUT";
        case SMOOTHAF_EPISODE_RESULT_BLOCKED: return "BLOCKED";
        case SMOOTHAF_EPISODE_RESULT_LENS_FAILED: return "LENS_FAILED";
        case SMOOTHAF_EPISODE_RESULT_NO_IMPROVEMENT: return "NO_IMPROVEMENT";
        case SMOOTHAF_EPISODE_RESULT_CANCELLED: return "CANCELLED";
        case SMOOTHAF_EPISODE_RESULT_NO_MOVEMENT: return "NO_MOVEMENT";
        case SMOOTHAF_EPISODE_RESULT_SEARCH_LIMIT: return "SEARCH_LIMIT";
        case SMOOTHAF_EPISODE_RESULT_DRY_RUN: return "DRY_RUN";
        default: return "NONE";
    }
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
        case SMOOTHAF_LOG_RECORDING: return "RECORDING";
        case SMOOTHAF_LOG_EPISODE: return "EPISODE";
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
