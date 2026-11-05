/**
 * SmoothAF — Persistent Logger (Phase 3A)
 *
 * Circular RAM buffer for structured AF episode logging.
 * Zero dynamic allocation, non-blocking producer, deterministic output.
 * Multiple producers are serialized by a bounded try-lock; the consumer never
 * performs producer-side locking or FIO. ARM/EABI5 compatible.
 */

#ifndef SMOOTHAF_LOGGER_H
#define SMOOTHAF_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ============================================================
 * Configuration (compile-time, adjustable for host tests)
 * ============================================================ */

/* Ring buffer capacity: power of 2 for fast modulo.
 * 256 entries = 256 * 40 bytes = 10.24 KB persistent RAM.
 * Host tests can override via -DSMOOTHAF_LOG_BUF_ENTRIES=1024. */
#ifndef SMOOTHAF_LOG_BUF_ENTRIES
#define SMOOTHAF_LOG_BUF_ENTRIES    256
#endif

#define SMOOTHAF_LOG_MAX_PAYLOAD    6       /* int32_t payload slots per record */

/* Fixed consumer line buffer: 4 KB total (16 lines x 256 bytes).
 * Eliminates large stack allocation in consume_batch. */
#define SMOOTHAF_LOG_LINE_MAX       256
#define SMOOTHAF_LOG_BATCH_LINES    16
#define SMOOTHAF_LOG_BATCH_BUF_SIZE (SMOOTHAF_LOG_BATCH_LINES * SMOOTHAF_LOG_LINE_MAX)

/* ============================================================
 * Compile-time size verification (after struct definitions)
 * ============================================================ */

#define SMOOTHAF_LOG_STATIC_ASSERT(cond, name) \
    typedef char name[(cond) ? 1 : -1]

SMOOTHAF_LOG_STATIC_ASSERT((SMOOTHAF_LOG_BUF_ENTRIES & (SMOOTHAF_LOG_BUF_ENTRIES - 1)) == 0,
                           smoothaf_log_buf_entries_power_of_2);

/* ============================================================
 * Event Type Definitions (prefixed to avoid conflict with diagnostics)
 * ============================================================ */

typedef enum smoothaf_persist_log_event_type {
    SMOOTHAF_PERSIST_LOG_NONE              = 0,

    /* Session lifecycle */
    SMOOTHAF_PERSIST_LOG_SESSION_START     = 1,   /* Build ID, config, mode */
    SMOOTHAF_PERSIST_LOG_SESSION_END       = 2,   /* Reason, duration, episodes */

    /* Episode lifecycle */
    SMOOTHAF_PERSIST_LOG_EPISODE_START     = 3,   /* Episode ID, algorithm, baseline */
    SMOOTHAF_PERSIST_LOG_EPISODE_END       = 4,   /* Result, best/final score/pos, cmds */

    /* Reference qualification */
    SMOOTHAF_PERSIST_LOG_REFERENCE_SET     = 5,   /* Type (PROV/VAL), score, position */

    /* Auto-probe phase */
    SMOOTHAF_PERSIST_LOG_AUTO_PROBE        = 6,   /* Phase, dir, score, delta, lighting */

    /* Focus command + timing */
    SMOOTHAF_PERSIST_LOG_COMMAND           = 7,   /* Cmd#, dir, step, lens_ms, settle_ms, sample_wait_ms */

    /* Measurement capture */
    SMOOTHAF_PERSIST_LOG_MEASUREMENT       = 8,   /* Score, mean, SAD, lap, valid, confident */

    /* Controller state machine */
    SMOOTHAF_PERSIST_LOG_CONTROLLER        = 9,   /* State, action, confidence, best_score */

    /* Termination conditions */
    SMOOTHAF_PERSIST_LOG_TIMEOUT           = 10,  /* Phase at timeout, budget_used_ms, cmds_executed */
    SMOOTHAF_PERSIST_LOG_LIMIT             = 11,  /* Limit type, commands_used */
    SMOOTHAF_PERSIST_LOG_CANCELLED         = 12,  /* STOP/PANIC reason, cmds_executed */
    SMOOTHAF_PERSIST_LOG_SAFETY_PAUSED     = 13,  /* Reason, remaining_ms */

    /* Environmental / diagnostic */
    SMOOTHAF_PERSIST_LOG_ENVIRONMENTAL     = 14,  /* Sub-type, value1, value2 */

    /* Logger self-diagnostics */
    SMOOTHAF_PERSIST_LOG_WRITE_ERROR_EVT   = 15,  /* Dropped count, error code */
} smoothaf_persist_log_event_type_e;

/* Reference qualification types */
typedef enum smoothaf_persist_log_ref_type {
    SMOOTHAF_PERSIST_LOG_REF_NONE      = 0,
    SMOOTHAF_PERSIST_LOG_REF_PROVISIONAL = 1,
    SMOOTHAF_PERSIST_LOG_REF_VALID       = 2,
} smoothaf_persist_log_ref_type_e;

/* Termination result codes (match smoothaf_episode_result) */
typedef enum smoothaf_persist_log_result {
    SMOOTHAF_PERSIST_LOG_RESULT_NONE             = 0,
    SMOOTHAF_PERSIST_LOG_RESULT_IMPROVED         = 1,
    SMOOTHAF_PERSIST_LOG_RESULT_WORSE            = 2,
    SMOOTHAF_PERSIST_LOG_RESULT_UNCERTAIN        = 3,
    SMOOTHAF_PERSIST_LOG_RESULT_CONVERGED        = 4,
    SMOOTHAF_PERSIST_LOG_RESULT_TIMEOUT          = 5,
    SMOOTHAF_PERSIST_LOG_RESULT_BLOCKED          = 6,
    SMOOTHAF_PERSIST_LOG_RESULT_LENS_FAILED      = 7,
    SMOOTHAF_PERSIST_LOG_RESULT_NO_IMPROVEMENT   = 8,
    SMOOTHAF_PERSIST_LOG_RESULT_CANCELLED        = 9,
    SMOOTHAF_PERSIST_LOG_RESULT_NO_MOVEMENT      = 10,
    SMOOTHAF_PERSIST_LOG_RESULT_SEARCH_LIMIT     = 11,
    SMOOTHAF_PERSIST_LOG_RESULT_DRY_RUN          = 12,
    SMOOTHAF_PERSIST_LOG_RESULT_BEST_RECOVERED   = 13,
    SMOOTHAF_PERSIST_LOG_RESULT_BEST_NOT_RECOVERED = 14,
} smoothaf_persist_log_result_e;

/* STOP/PANIC cancellation reasons */
typedef enum smoothaf_persist_log_cancel_reason {
    SMOOTHAF_PERSIST_LOG_CANCEL_NONE     = 0,
    SMOOTHAF_PERSIST_LOG_CANCEL_STOP     = 1,
    SMOOTHAF_PERSIST_LOG_CANCEL_PANIC    = 2,
    SMOOTHAF_PERSIST_LOG_CANCEL_CONFIG   = 3,
} smoothaf_persist_log_cancel_reason_e;

/* Safety pause reasons */
typedef enum smoothaf_persist_log_safety_reason {
    SMOOTHAF_PERSIST_LOG_SAFETY_NONE          = 0,
    SMOOTHAF_PERSIST_LOG_SAFETY_COOLDOWN      = 1,
    SMOOTHAF_PERSIST_LOG_SAFETY_FAILURES      = 2,
    SMOOTHAF_PERSIST_LOG_SAFETY_BLUR          = 3,
    SMOOTHAF_PERSIST_LOG_SAFETY_ACQUISITION   = 4,
} smoothaf_persist_log_safety_reason_e;

/* Limit types */
typedef enum smoothaf_persist_log_limit_type {
    SMOOTHAF_PERSIST_LOG_LIMIT_NONE         = 0,
    SMOOTHAF_PERSIST_LOG_LIMIT_EPISODE_CMDS = 1,
    SMOOTHAF_PERSIST_LOG_LIMIT_SESSION_CMDS = 2,
    SMOOTHAF_PERSIST_LOG_LIMIT_REVERSALS    = 3,
} smoothaf_persist_log_limit_type_e;

/* ============================================================
 * Log Record Structure (fixed size, no dynamic allocation)
 * 36 bytes exactly: naturally aligned for ARM EABI
 * ============================================================ */

struct smoothaf_persist_log_record {
    uint32_t timestamp_ms;              /* Monotonic ms (get_ms_clock equivalent) */
    uint16_t event_type;                /* smoothaf_persist_log_event_type_e */
    uint16_t episode_id;                /* Current episode (0 = session-level) */
    uint32_t sequence;                  /* Monotonic sequence for ordering */
    int32_t  payload[SMOOTHAF_LOG_MAX_PAYLOAD];  /* Typed per event_type */
};

/* ============================================================
 * Circular Buffer State
 * ============================================================ */

struct smoothaf_persist_log_buffer {
    struct smoothaf_persist_log_record ring[SMOOTHAF_LOG_BUF_ENTRIES];
    volatile uint32_t head;              /* Next write index (published producer) */
    volatile uint32_t tail;              /* Next committed read index */
    volatile uint32_t reserved_tail;     /* Oldest slot reserved by consumer */
    volatile bool consumer_pending;      /* A formatted batch owns the range */
    volatile uint32_t dropped_count;     /* Records lost due to full buffer */
    volatile uint32_t published_count;   /* Records published by producers */
    volatile uint32_t flushed_count;     /* Records confirmed by a backend */
    uint32_t sequence;                   /* Monotonic sequence for ordering */
    char build_id[32];                   /* Session build ID, copied at start */
    volatile bool enabled;               /* Global enable flag (default false) */
    volatile uint32_t producer_lock;     /* Multi-producer try-lock: 0/1 */
    volatile uint32_t producer_contention_count; /* Producers unable to enter */
};

/* Compile-time size verification (after struct definitions) */
#define SMOOTHAF_PERSIST_LOG_RECORD_SIZE    sizeof(struct smoothaf_persist_log_record)
#define SMOOTHAF_PERSIST_LOG_RING_SIZE      (SMOOTHAF_PERSIST_LOG_RECORD_SIZE * SMOOTHAF_LOG_BUF_ENTRIES)
#define SMOOTHAF_PERSIST_LOG_BUFFER_SIZE    sizeof(struct smoothaf_persist_log_buffer)

/* ============================================================
 * Write Backend Abstraction (Phase 2: simulated, Phase 3: FIO)
 * ============================================================ */

typedef enum smoothaf_persist_log_write_result {
    SMOOTHAF_PERSIST_LOG_WRITE_OK       = 0,
    SMOOTHAF_PERSIST_LOG_WRITE_FULL     = 1,   /* Buffer full, data retained */
    SMOOTHAF_PERSIST_LOG_WRITE_ERR      = 2,   /* I/O error, data retained */
    SMOOTHAF_PERSIST_LOG_WRITE_PARTIAL  = 3,   /* Partial write, remaining retained */
} smoothaf_persist_log_write_result_e;

/* Write backend function signature */
typedef smoothaf_persist_log_write_result_e (*smoothaf_persist_log_write_fn)(
    const char *data, uint32_t len, void *ctx);

/* Backend context (opaque to logger core) */
struct smoothaf_persist_log_backend {
    smoothaf_persist_log_write_fn write_fn;
    void *ctx;
    uint32_t max_batch_bytes;         /* Max bytes per write call */
};

/* Default simulated backend (host tests) */
extern struct smoothaf_persist_log_backend smoothaf_persist_log_sim_backend;

/* ============================================================
 * Consumer Batch Output (fixed 4 KB buffer)
 * ============================================================ */

struct smoothaf_persist_log_batch {
    char lines[SMOOTHAF_LOG_BATCH_LINES][SMOOTHAF_LOG_LINE_MAX];
    uint32_t count;
    uint32_t bytes_total;
    uint32_t source_tail;                /* Ring tail used when formatting */
    uint32_t record_count;               /* Records reserved by this batch */
    uint32_t next_line;                  /* Next line to write after a retry */
    bool truncated;                      /* True if buffer full during format */
};

/* ============================================================
 * Public API (prefixed to avoid conflict with diagnostics)
 * ============================================================ */

/* Initialize logger (call once at startup) */
void smoothaf_persist_logger_init(struct smoothaf_persist_log_buffer *buf);

/* Enable/disable logging at runtime (default: disabled) */
void smoothaf_persist_logger_set_enabled(struct smoothaf_persist_log_buffer *buf, bool enabled);
bool smoothaf_persist_logger_is_enabled(const struct smoothaf_persist_log_buffer *buf);

/* Non-blocking record insertion (producer path — must be fast, no allocation)
 * Returns true if recorded, false if buffer full (dropped_count incremented).
 * Must be callable from AF critical path. */
bool smoothaf_persist_log_record(struct smoothaf_persist_log_buffer *buf,
                          uint32_t timestamp_ms,
                          uint16_t event_type,
                          uint16_t episode_id,
                          const int32_t *payload);

/* Convenience helpers for each event type (payload typing documented per type) */
bool smoothaf_persist_log_session_start(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 const char *build_id, uint32_t timeout_ms,
                                 uint8_t max_cmds, uint8_t auto_probe_enabled);
bool smoothaf_persist_log_session_end(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint8_t reason, uint32_t duration_ms,
                               uint16_t episodes_completed);
bool smoothaf_persist_log_limit(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                          uint16_t episode_id, uint8_t limit_type,
                          uint32_t commands_used);
bool smoothaf_persist_log_episode_start(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 uint16_t episode_id, uint8_t algorithm,
                                 int32_t baseline_score);
bool smoothaf_persist_log_episode_end(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, uint8_t result,
                               int32_t best_score, int32_t best_pos,
                               int32_t final_score, int32_t final_pos,
                               uint8_t commands_used);
bool smoothaf_persist_log_command(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                           uint16_t episode_id, uint8_t cmd_index,
                           int8_t direction, uint8_t step,
                           uint32_t lens_ms, uint32_t settle_ms,
                           uint32_t sample_wait_ms);
bool smoothaf_persist_log_measurement(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, int32_t score,
                               int32_t mean, int32_t sad, int32_t laplacian,
                               uint8_t valid, uint8_t confident);
bool smoothaf_persist_log_timeout(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                           uint16_t episode_id, uint8_t phase,
                           uint32_t budget_used_ms, uint8_t cmds_executed);
bool smoothaf_persist_log_safety_paused(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                 uint16_t episode_id, uint8_t reason,
                                 uint32_t remaining_ms);
bool smoothaf_persist_log_reference_set(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t reference_type,
                                  int32_t score, int32_t position);
bool smoothaf_persist_log_auto_probe(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t phase, int8_t direction,
                                  int32_t score, int32_t delta, uint8_t lighting_valid);
bool smoothaf_persist_log_controller(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t state, uint8_t action,
                                  uint8_t confidence, int32_t best_score);
bool smoothaf_persist_log_environmental(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint8_t subtype,
                                  int32_t value1, int32_t value2);
bool smoothaf_persist_log_write_error(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                                  uint16_t episode_id, uint32_t dropped_count,
                                  int32_t error_code);
bool smoothaf_persist_log_cancelled(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                             uint16_t episode_id, uint8_t cancel_reason,
                             uint8_t cmds_executed);

/* Consumer: extract available records as formatted lines (up to batch limit)
 * Returns number of records consumed. Non-blocking. */
uint32_t smoothaf_persist_log_consume_batch(struct smoothaf_persist_log_buffer *buf,
                                     struct smoothaf_persist_log_batch *out);

/* Flush a formatted batch via backend (simulated write).
 * Returns write result. Non-blocking, bounded. On failure, the batch remains
 * retryable and the ring tail is restored to the batch source position. */
smoothaf_persist_log_write_result_e smoothaf_persist_log_flush_batch(
    struct smoothaf_persist_log_buffer *buf,
    struct smoothaf_persist_log_batch *batch,
    struct smoothaf_persist_log_backend *backend);

/* Abandon an in-flight consumer reservation without touching the card. */
void smoothaf_persist_log_abort_batch(struct smoothaf_persist_log_buffer *buf,
                                      struct smoothaf_persist_log_batch *batch);

/* Get current dropped count (for telemetry) */
uint32_t smoothaf_persist_log_get_dropped(const struct smoothaf_persist_log_buffer *buf);

/* Reset dropped counter (e.g., at session start) */
void smoothaf_persist_log_reset_dropped(struct smoothaf_persist_log_buffer *buf);

/* Get buffer fill level (0..SMOOTHAF_LOG_BUF_ENTRIES) for monitoring */
uint32_t smoothaf_persist_log_get_fill(const struct smoothaf_persist_log_buffer *buf);
uint32_t smoothaf_persist_log_get_published(const struct smoothaf_persist_log_buffer *buf);
uint32_t smoothaf_persist_log_get_flushed(const struct smoothaf_persist_log_buffer *buf);

/* Check if buffer is empty/full */
bool smoothaf_persist_log_is_empty(const struct smoothaf_persist_log_buffer *buf);
bool smoothaf_persist_log_is_full(const struct smoothaf_persist_log_buffer *buf);

/* Simulated "STOP/PANIC" signal — interrupts producer, marks pending flush
 * Does NOT perform any I/O. Consumer must handle pending data. */
void smoothaf_persist_log_signal_stop(struct smoothaf_persist_log_buffer *buf, uint32_t ts,
                               uint16_t episode_id, uint8_t cancel_reason,
                               uint8_t cmds_executed);

/* ============================================================
 * Mock / Test Support
 * ============================================================ */

/* Inject a mock timestamp source (for wrap-around tests) */
void smoothaf_persist_log_set_mock_clock(uint32_t (*mock_get_ms)(void));
/* Restore real clock (host simulation uses get_ms_clock equivalent) */
void smoothaf_persist_log_clear_mock_clock(void);

/* Simulate slow consumer (delay between consume calls) */
void smoothaf_persist_log_set_consumer_delay_ms(uint32_t delay_ms);

/* Simulate write failure (next flush_batch returns error) */
void smoothaf_persist_log_set_write_fail_next(bool fail);

/* ============================================================
 * Payload Layout Documentation (per event_type)
 * ============================================================
 *
 * SESSION_START:      [0]=build_id stored in buffer, [1]=timeout_ms, [2]=max_cmds, [3]=auto_probe
 * SESSION_END:        [0]=reason, [1]=duration_ms, [2]=episodes_completed
 * EPISODE_START:      [0]=algorithm, [1]=baseline_score
 * EPISODE_END:        [0]=result, [1]=best_score, [2]=best_pos, [3]=final_score,
 *                     [4]=final_pos, [5]=commands_used
 * REFERENCE_SET:      [0]=ref_type, [1]=score, [2]=position
 * AUTO_PROBE:         [0]=phase, [1]=direction, [2]=score, [3]=delta, [4]=lighting_valid
 * COMMAND:            [0]=cmd_index, [1]=direction, [2]=step, [3]=lens_ms,
 *                     [4]=settle_ms, [5]=sample_wait_ms
 * MEASUREMENT:        [0]=score, [1]=mean, [2]=sad, [3]=laplacian, [4]=valid, [5]=confident
 * CONTROLLER:         [0]=state, [1]=action, [2]=confidence, [3]=best_score
 * TIMEOUT:            [0]=phase, [1]=budget_used_ms, [2]=cmds_executed
 * LIMIT:              [0]=limit_type, [1]=commands_used
 * CANCELLED:          [0]=cancel_reason, [1]=cmds_executed
 * SAFETY_PAUSED:      [0]=reason, [1]=remaining_ms
 * ENVIRONMENTAL:      [0]=sub_type, [1]=value1, [2]=value2
 * WRITE_ERROR:        [0]=dropped_count, [1]=error_code
 *
 * The SESSION_START build ID is copied into the logger state so formatting and
 * persistence never depend on a truncated pointer or caller-owned storage.
 */

#endif /* SMOOTHAF_LOGGER_H */