/**
 * SmoothAF — Persistent Logger Implementation (Phase 3A)
 *
 * Circular buffer, deterministic formatting, zero dynamic allocation.
 * Multi-producer / single-consumer with a bounded producer gate and barriers.
 * Simulated write backend for host tests.
 */

#include "smoothaf_logger.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 * Memory barriers and bounded atomics
 * ============================================================
 * The 600D Legacy loader does not provide libgcc __sync helpers. ARM9 has
 * SWP, so use it directly on target; host GCC keeps the stronger built-ins.
 */
#if defined(__arm__) || defined(__ARM_ARCH)
#define SMOOTHAF_PERSIST_LOG_BARRIER() \
    do { unsigned long smoothaf_zero = 0; \
         __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" \
                          : : "r"(smoothaf_zero) : "memory"); } while (0)
#else
#define SMOOTHAF_PERSIST_LOG_BARRIER() __sync_synchronize()
#endif

static void logger_atomic_inc(volatile uint32_t *value)
{
    /* Counters are advisory; record publication remains protected by the gate. */
    (*value)++;
}

static bool producer_try_lock(struct smoothaf_persist_log_buffer *buf)
{
#if defined(__arm__) || defined(__ARM_ARCH)
    uint32_t previous;
    uint32_t one = 1u;
    __asm__ volatile("swp %0, %2, [%1]"
                     : "=&r"(previous) : "r"(&buf->producer_lock), "r"(one)
                     : "memory");
    if (previous != 0u) {
        logger_atomic_inc(&buf->producer_contention_count);
        return false;
    }
    return true;
#elif defined(__GNUC__)
    if (__sync_lock_test_and_set(&buf->producer_lock, 1u) != 0u) {
        logger_atomic_inc(&buf->producer_contention_count);
        return false;
    }
    return true;
#else
    if (buf->producer_lock != 0u) {
        logger_atomic_inc(&buf->producer_contention_count);
        return false;
    }
    buf->producer_lock = 1u;
    return true;
#endif
}

static void producer_unlock(struct smoothaf_persist_log_buffer *buf)
{
    SMOOTHAF_PERSIST_LOG_BARRIER();
#if defined(__arm__) || defined(__ARM_ARCH)
    buf->producer_lock = 0u;
#elif defined(__GNUC__)
    __sync_lock_release(&buf->producer_lock);
#else
    buf->producer_lock = 0u;
#endif
}

/* ============================================================
 * Internal State (Mock Support)
 * ============================================================ */

static uint32_t (*s_mock_get_ms)(void) = NULL;
static uint32_t s_consumer_delay_ms = 0;
static bool s_write_fail_next = false;

/* Default clock function (host simulation) */
static uint32_t default_get_ms(void) {
    static uint32_t counter = 1000000;
    return counter += 10;
}

/* ============================================================
 * Clock Accessor
 * ============================================================ */

static inline uint32_t get_timestamp_ms(void) {
    if (s_mock_get_ms) return s_mock_get_ms();
    return default_get_ms();
}

/* ============================================================
 * Buffer Helpers
 * ============================================================ */

static inline uint32_t buf_mask(const struct smoothaf_persist_log_buffer *buf) {
    (void)buf; /* SMOOTHAF_LOG_BUF_ENTRIES is compile-time constant */
    return SMOOTHAF_LOG_BUF_ENTRIES - 1;
}

static inline uint32_t buf_producer_tail(const struct smoothaf_persist_log_buffer *buf) {
    if (buf->consumer_pending) {
        SMOOTHAF_PERSIST_LOG_BARRIER();
        return buf->reserved_tail;
    }
    SMOOTHAF_PERSIST_LOG_BARRIER();
    return buf->tail;
}

static inline bool buf_is_full(const struct smoothaf_persist_log_buffer *buf) {
    uint32_t next_head = (buf->head + 1) & buf_mask(buf);
    return next_head == buf_producer_tail(buf);
}

static inline bool buf_is_empty(const struct smoothaf_persist_log_buffer *buf) {
    return buf->head == buf->tail;
}

static inline uint32_t buf_fill(const struct smoothaf_persist_log_buffer *buf) {
    return (buf->head - buf->tail) & buf_mask(buf);
}

/* ============================================================
 * Core Record Insertion (Producer Path)
 * ============================================================ */

bool smoothaf_persist_log_record(struct smoothaf_persist_log_buffer *buf,
                          uint32_t timestamp_ms,
                          uint16_t event_type,
                          uint16_t episode_id,
                          const int32_t *payload)
{
    if (!buf || !buf->enabled) return true;  /* Silent no-op when disabled */
    if (!producer_try_lock(buf)) {
        logger_atomic_inc(&buf->dropped_count);
        return false;
    }

    uint32_t next_head = (buf->head + 1) & buf_mask(buf);
    if (next_head == buf_producer_tail(buf)) {
        /* Buffer full — drop record, increment counter */
        logger_atomic_inc(&buf->dropped_count);
        producer_unlock(buf);
        return false;
    }

    struct smoothaf_persist_log_record *rec = &buf->ring[buf->head];
    rec->timestamp_ms = timestamp_ms;
    rec->event_type = event_type;
    rec->episode_id = episode_id;
    rec->sequence = buf->sequence++;
    if (payload) {
        memcpy(rec->payload, payload, sizeof(rec->payload));
    } else {
        memset(rec->payload, 0, sizeof(rec->payload));
    }

    /* Publish the complete payload only after it has been written. */
    SMOOTHAF_PERSIST_LOG_BARRIER();
    buf->head = next_head;
    producer_unlock(buf);
    return true;
}

/* ============================================================
 * Convenience Helpers (Payload Construction)
 * ============================================================ */

bool smoothaf_persist_log_session_start(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 const char *build_id, uint32_t timeout_ms,
                                 uint8_t max_cmds, uint8_t auto_probe_enabled)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        0,
        (int32_t)timeout_ms,
        (int32_t)max_cmds,
        (int32_t)auto_probe_enabled,
        0, 0
    };
    if (buf && build_id) {
        strncpy(buf->build_id, build_id, sizeof(buf->build_id) - 1);
        buf->build_id[sizeof(buf->build_id) - 1] = '\0';
    }
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_SESSION_START, 0, payload);
}

bool smoothaf_persist_log_session_end(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint8_t reason, uint32_t duration_ms,
                               uint16_t episodes_completed)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)reason,
        (int32_t)duration_ms,
        (int32_t)episodes_completed,
        0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_SESSION_END, 0, payload);
}

bool smoothaf_persist_log_limit(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                          uint16_t episode_id, uint8_t limit_type,
                          uint32_t commands_used)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)limit_type, (int32_t)commands_used, 0, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_LIMIT,
                                       episode_id, payload);
}

bool smoothaf_persist_log_episode_start(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 uint16_t episode_id, uint8_t algorithm,
                                 int32_t baseline_score)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)algorithm,
        baseline_score,
        0, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_EPISODE_START, episode_id, payload);
}

bool smoothaf_persist_log_episode_end(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, uint8_t result,
                               int32_t best_score, int32_t best_pos,
                               int32_t final_score, int32_t final_pos,
                               uint8_t commands_used)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)result,
        best_score,
        best_pos,
        final_score,
        final_pos,
        (int32_t)commands_used
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_EPISODE_END, episode_id, payload);
}

bool smoothaf_persist_log_command(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                           uint16_t episode_id, uint8_t cmd_index,
                           int8_t direction, uint8_t step,
                           uint32_t lens_ms, uint32_t settle_ms,
                           uint32_t sample_wait_ms)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)cmd_index,
        (int32_t)direction,
        (int32_t)step,
        (int32_t)lens_ms,
        (int32_t)settle_ms,
        (int32_t)sample_wait_ms
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_COMMAND, episode_id, payload);
}

bool smoothaf_persist_log_measurement(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, int32_t score,
                               int32_t mean, int32_t sad, int32_t laplacian,
                               uint8_t valid, uint8_t confident)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        score, mean, sad, laplacian,
        (int32_t)valid, (int32_t)confident
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_MEASUREMENT, episode_id, payload);
}

bool smoothaf_persist_log_timeout(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                           uint16_t episode_id, uint8_t phase,
                           uint32_t budget_used_ms, uint8_t cmds_executed)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)phase,
        (int32_t)budget_used_ms,
        (int32_t)cmds_executed,
        0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_TIMEOUT, episode_id, payload);
}

bool smoothaf_persist_log_safety_paused(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 uint16_t episode_id, uint8_t reason,
                                 uint32_t remaining_ms)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)reason,
        (int32_t)remaining_ms,
        0, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_SAFETY_PAUSED, episode_id, payload);
}

bool smoothaf_persist_log_reference_set(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t reference_type,
                                  int32_t score, int32_t position)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)reference_type, score, position, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_REFERENCE_SET,
                                       episode_id, payload);
}

bool smoothaf_persist_log_auto_probe(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t phase, int8_t direction,
                                  int32_t score, int32_t delta, uint8_t lighting_valid)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)phase, (int32_t)direction, score, delta,
        (int32_t)lighting_valid, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_AUTO_PROBE,
                                       episode_id, payload);
}

bool smoothaf_persist_log_controller(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t state, uint8_t action,
                                  uint8_t confidence, int32_t best_score)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)state, (int32_t)action, (int32_t)confidence, best_score, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_CONTROLLER,
                                       episode_id, payload);
}

bool smoothaf_persist_log_environmental(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t subtype,
                                  int32_t value1, int32_t value2)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)subtype, value1, value2, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_ENVIRONMENTAL,
                                       episode_id, payload);
}

bool smoothaf_persist_log_write_error(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint32_t dropped_count,
                                  int32_t error_code)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)dropped_count, error_code, 0, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_WRITE_ERROR_EVT,
                                       episode_id, payload);
}

bool smoothaf_persist_log_cancelled(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                             uint16_t episode_id, uint8_t cancel_reason,
                             uint8_t cmds_executed)
{
    int32_t payload[SMOOTHAF_LOG_MAX_PAYLOAD] = {
        (int32_t)cancel_reason,
        (int32_t)cmds_executed,
        0, 0, 0, 0
    };
    return smoothaf_persist_log_record(buf, ts, SMOOTHAF_PERSIST_LOG_CANCELLED, episode_id, payload);
}

/* ============================================================
 * Formatting (Consumer Path — Simulated Batch Write)
 * ============================================================ */

static const char *event_name(uint16_t type) {
    switch (type) {
        case SMOOTHAF_PERSIST_LOG_SESSION_START:    return "SESSION_START";
        case SMOOTHAF_PERSIST_LOG_SESSION_END:      return "SESSION_END";
        case SMOOTHAF_PERSIST_LOG_EPISODE_START:    return "EPISODE_START";
        case SMOOTHAF_PERSIST_LOG_EPISODE_END:      return "EPISODE_END";
        case SMOOTHAF_PERSIST_LOG_REFERENCE_SET:    return "REFERENCE_SET";
        case SMOOTHAF_PERSIST_LOG_AUTO_PROBE:       return "AUTO_PROBE";
        case SMOOTHAF_PERSIST_LOG_COMMAND:          return "COMMAND";
        case SMOOTHAF_PERSIST_LOG_MEASUREMENT:      return "MEASUREMENT";
        case SMOOTHAF_PERSIST_LOG_CONTROLLER:       return "CONTROLLER";
        case SMOOTHAF_PERSIST_LOG_TIMEOUT:          return "TIMEOUT";
        case SMOOTHAF_PERSIST_LOG_LIMIT:            return "LIMIT";
        case SMOOTHAF_PERSIST_LOG_CANCELLED:        return "CANCELLED";
        case SMOOTHAF_PERSIST_LOG_SAFETY_PAUSED:    return "SAFETY_PAUSED";
        case SMOOTHAF_PERSIST_LOG_ENVIRONMENTAL:    return "ENVIRONMENTAL";
        case SMOOTHAF_PERSIST_LOG_WRITE_ERROR_EVT:  return "WRITE_ERROR";
        default:                            return "UNKNOWN";
    }
}

static int format_record(const struct smoothaf_persist_log_record *rec, char *out, size_t maxlen) {
    int len = snprintf(out, maxlen, "%lu\t%s\t%u",
                       (unsigned long)rec->timestamp_ms, event_name(rec->event_type), rec->episode_id);
    if (len < 0 || (size_t)len >= maxlen) return -1;

    for (int i = 0; i < SMOOTHAF_LOG_MAX_PAYLOAD; i++) {
        int n = snprintf(out + len, maxlen - (size_t)len, "\t%ld", (long)rec->payload[i]);
        if (n < 0 || len + n >= (int)maxlen) return -1;
        len += n;
    }
    return len;
}

uint32_t smoothaf_persist_log_consume_batch(struct smoothaf_persist_log_buffer *buf,
                                     struct smoothaf_persist_log_batch *out)
{
    if (!buf || !out) return 0;

    /* Simulate consumer delay for testing */
    if (s_consumer_delay_ms) {
        /* In host test, we can't actually sleep without threads. */
    }

    /* A caller may use consume_batch as a RAM-only drain without calling
     * flush_batch. Treat the previous reservation as committed before making
     * the next batch; a failed flush keeps using the same batch object. */
    /* A direct RAM-only consumer commits the previous snapshot. The FIO
     * service handles a pending snapshot through flush_batch instead. */
    if (buf->consumer_pending) {
        /* A direct consumer call starts a new snapshot. The FIO service does
         * not call this function while a previous batch is pending. */
        buf->consumer_pending = false;
        buf->reserved_tail = buf->tail;
    }

    uint32_t consumed = 0;
    out->source_tail = buf->tail;
    out->record_count = 0;
    out->next_line = 0;
    out->count = 0;
    out->bytes_total = 0;
    out->truncated = false;

    /* Reserve the current tail while formatting. Producers may continue into
     * free slots, but cannot overwrite the records owned by this batch. */
    buf->reserved_tail = buf->tail;
    SMOOTHAF_PERSIST_LOG_BARRIER();
    buf->consumer_pending = true;

    /* Process up to batch limit or until the producer snapshot is reached. */
    uint32_t cursor = buf->tail;
    uint32_t published_head = buf->head;
    while (cursor != published_head && out->count < SMOOTHAF_LOG_BATCH_LINES) {
        /* Memory barrier: ensure we see the record before reading */
        SMOOTHAF_PERSIST_LOG_BARRIER();

        const struct smoothaf_persist_log_record *rec = &buf->ring[cursor];
        int len = format_record(rec, out->lines[out->count], SMOOTHAF_LOG_LINE_MAX);
        if (len < 0) {
            out->truncated = true;
            break;
        }
        out->bytes_total += (uint32_t)len;
        out->count++;
        consumed++;
        out->record_count++;
        cursor = (cursor + 1) & buf_mask(buf);
        buf->tail = cursor;
    }

    /* Allow producers to use slots after the formatted snapshot. If the
     * backend fails, flush_batch restores the source tail and reservation. */
    if (consumed != 0) buf->reserved_tail = cursor;
    if (consumed == 0) {
        buf->consumer_pending = false;
    }
    return consumed;
}

/* ============================================================
 * Simulated Write Backend (Host Tests)
 * ============================================================ */

static smoothaf_persist_log_write_result_e sim_write_fn(const char *data, uint32_t len, void *ctx) {
    (void)data; (void)len; (void)ctx;
    if (s_write_fail_next) {
        s_write_fail_next = false;
        return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
    }
    return SMOOTHAF_PERSIST_LOG_WRITE_OK;
}

struct smoothaf_persist_log_backend smoothaf_persist_log_sim_backend = {
    .write_fn = sim_write_fn,
    .ctx = NULL,
    .max_batch_bytes = SMOOTHAF_LOG_BATCH_BUF_SIZE,
};

/* ============================================================
 * Flush Batch via Backend
 * ============================================================ */

smoothaf_persist_log_write_result_e smoothaf_persist_log_flush_batch(
    struct smoothaf_persist_log_buffer *buf,
    struct smoothaf_persist_log_batch *batch,
    struct smoothaf_persist_log_backend *backend)
{
    if (!buf || !batch || !backend || !backend->write_fn) {
        return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
    }

    if (!buf->consumer_pending) return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
    if (batch->count == 0) {
        buf->consumer_pending = false;
        return SMOOTHAF_PERSIST_LOG_WRITE_OK;
    }

    /* The batch is already bounded to 4 KB and each line is NUL terminated.
     * Write one line plus newline at a time so the consumer never allocates a
     * second 4 KB stack buffer. If a later line fails, only that line and the
     * remaining lines are restored for retry; already-written lines stay
     * consumed. */
    if (backend->max_batch_bytes == 0 ||
        batch->bytes_total + batch->count > backend->max_batch_bytes) {
        buf->consumer_pending = true;
        return SMOOTHAF_PERSIST_LOG_WRITE_PARTIAL;
    }

    uint32_t current_tail = batch->source_tail;
    if (batch->next_line > batch->count) {
        batch->next_line = batch->count;
    }
    current_tail = (current_tail + batch->next_line) & buf_mask(buf);

    for (uint32_t i = batch->next_line; i < batch->count; i++) {
        size_t line_len = strlen(batch->lines[i]);
        if (line_len >= SMOOTHAF_LOG_LINE_MAX) {
            buf->tail = current_tail;
            buf->consumer_pending = true;
            return SMOOTHAF_PERSIST_LOG_WRITE_PARTIAL;
        }

        char line_with_newline[SMOOTHAF_LOG_LINE_MAX + 1];
        memcpy(line_with_newline, batch->lines[i], line_len);
        line_with_newline[line_len] = '\n';
        smoothaf_persist_log_write_result_e result = backend->write_fn(
            line_with_newline, (uint32_t)line_len + 1, backend->ctx);
        if (result != SMOOTHAF_PERSIST_LOG_WRITE_OK) {
            buf->tail = current_tail;
            buf->reserved_tail = current_tail;
            buf->consumer_pending = true;
            return result;
        }

        current_tail = (current_tail + 1) & buf_mask(buf);
        batch->next_line = i + 1;
        buf->tail = current_tail;
    }

    buf->consumer_pending = false;
    buf->tail = current_tail;
    buf->reserved_tail = current_tail;
    return SMOOTHAF_PERSIST_LOG_WRITE_OK;
}

void smoothaf_persist_log_abort_batch(struct smoothaf_persist_log_buffer *buf,
                                      struct smoothaf_persist_log_batch *batch)
{
    if (!buf || !batch || !buf->consumer_pending) return;
    buf->tail = batch->source_tail;
    buf->reserved_tail = batch->source_tail;
    buf->consumer_pending = false;
    batch->next_line = 0;
}

/* ============================================================
 * Public API — State Access
 * ============================================================ */

void smoothaf_persist_logger_init(struct smoothaf_persist_log_buffer *buf) {
    if (!buf) return;
    memset(buf, 0, sizeof(*buf));
    buf->enabled = false;  /* Default OFF */
    buf->build_id[0] = '\0';
}

void smoothaf_persist_logger_set_enabled(struct smoothaf_persist_log_buffer *buf, bool enabled) {
    if (buf) buf->enabled = enabled;
}

bool smoothaf_persist_logger_is_enabled(const struct smoothaf_persist_log_buffer *buf) {
    return buf ? buf->enabled : false;
}

uint32_t smoothaf_persist_log_get_dropped(const struct smoothaf_persist_log_buffer *buf) {
    return buf ? buf->dropped_count : 0;
}

void smoothaf_persist_log_reset_dropped(struct smoothaf_persist_log_buffer *buf) {
    if (buf) buf->dropped_count = 0;
}

uint32_t smoothaf_persist_log_get_fill(const struct smoothaf_persist_log_buffer *buf) {
    return buf ? buf_fill(buf) : 0;
}

bool smoothaf_persist_log_is_empty(const struct smoothaf_persist_log_buffer *buf) {
    return buf ? buf_is_empty(buf) : true;
}

bool smoothaf_persist_log_is_full(const struct smoothaf_persist_log_buffer *buf) {
    return buf ? buf_is_full(buf) : false;
}

void smoothaf_persist_log_signal_stop(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, uint8_t cancel_reason,
                               uint8_t cmds_executed)
{
    if (!buf) return;
    /* Record cancellation event (non-blocking, may drop if full) */
    smoothaf_persist_log_cancelled(buf, ts, episode_id, cancel_reason, cmds_executed);
    /* Note: Does NOT flush. Consumer must drain remaining records. */
}

/* ============================================================
 * Mock / Test Support
 * ============================================================ */

void smoothaf_persist_log_set_mock_clock(uint32_t (*mock_get_ms)(void)) {
    s_mock_get_ms = mock_get_ms;
}

void smoothaf_persist_log_clear_mock_clock(void) {
    s_mock_get_ms = NULL;
}

void smoothaf_persist_log_set_consumer_delay_ms(uint32_t delay_ms) {
    s_consumer_delay_ms = delay_ms;
}

void smoothaf_persist_log_set_write_fail_next(bool fail) {
    s_write_fail_next = fail;
}