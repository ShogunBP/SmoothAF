/**
 * SmoothAF Logger — FIO Backend (Phase 3A)
 *
 * File I/O backend using Magic Lantern's FIO API.
 * Disabled by default, no FIO calls in AF critical path.
 * 8.3 filenames, file rotation, error handling.
 */

#ifndef SMOOTHAF_LOGGER_FIO_H
#define SMOOTHAF_LOGGER_FIO_H

#include "smoothaf_logger.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ============================================================
 * FIO Backend Configuration
 * ============================================================ */

#define SMOOTHAF_LOG_FIO_MAX_FILES      9        /* smoothaf000.log .. smoothaf008.log */
#define SMOOTHAF_LOG_FIO_MAX_SIZE_KB    256      /* Per-file size limit (KB) */
#define SMOOTHAF_LOG_FIO_DIR            "ML/LOGS" /* Log directory */
#define SMOOTHAF_LOG_FIO_PATTERN        "SMTH%03d.LOG"  /* 8.3 filename pattern */

/* ML FIO_MAX_PATH_LENGTH fallback */
#ifndef FIO_MAX_PATH_LENGTH
#define FIO_MAX_PATH_LENGTH 256
#endif

/* ============================================================
 * FIO Backend State
 * ============================================================ */

struct smoothaf_log_fio_backend {
    struct smoothaf_persist_log_backend base;     /* Base backend interface */

    /* File state */
    FILE *current_file;                   /* Open file handle */
    char current_filename[FIO_MAX_PATH_LENGTH];  /* Current file name */
    uint32_t current_file_size;           /* Bytes written to current file */
    uint32_t file_index;                  /* Current file number (0..SMOOTHAF_LOG_FIO_MAX_FILES-1) */
    uint32_t total_files_written;         /* Total files created this session */

    /* Configuration */
    volatile bool enabled;                /* Effective state, FIO-task owned */
    volatile bool requested_enabled;      /* Menu/config request */
    volatile bool config_pending;         /* FIO task must apply request */
    uint32_t max_file_size_bytes;         /* Max size per file */
    uint32_t max_files;                   /* Max number of files */
    volatile uint32_t requested_max_file_size_bytes;
    volatile uint32_t requested_max_files;

    /* Error tracking */
    uint32_t write_errors;                /* Total write errors */
    uint32_t partial_writes;              /* Total partial writes */
    uint32_t file_full_count;             /* Times file size limit reached */
    uint32_t max_files_reached;           /* Times max files limit reached */
    int last_error;                       /* Last error code */

    /* Synchronization (for consumer task) */
    volatile bool flush_pending;          /* Flush requested */
    volatile bool close_pending;          /* Close requested */

    /* Resume a line after a short write followed by an error. The batch object
     * remains stable while the logger retries it. */
    bool retry_pending;
    uint32_t retry_signature;
    uint32_t retry_len;
    uint32_t retry_offset;
};

/* ============================================================
 * Public API
 * ============================================================ */

/* Initialize FIO backend (call once at startup) */
void smoothaf_log_fio_init(struct smoothaf_log_fio_backend *fio);

/* Enable/disable FIO backend at runtime (default: disabled) */
void smoothaf_log_fio_set_enabled(struct smoothaf_log_fio_backend *fio, bool enabled);
bool smoothaf_log_fio_is_enabled(const struct smoothaf_log_fio_backend *fio);

/* Configuration is published by menu/control code and applied only by the
 * consumer that owns FILE *. */
void smoothaf_log_fio_request_enabled(struct smoothaf_log_fio_backend *fio, bool enabled);
void smoothaf_log_fio_request_limits(struct smoothaf_log_fio_backend *fio,
                                     uint32_t max_file_size_bytes,
                                     uint32_t max_files);

/* Consumer calls this periodically to flush batches */
/* Returns true if work was done, false if idle */
bool smoothaf_log_fio_service(struct smoothaf_log_fio_backend *fio,
                              struct smoothaf_persist_log_buffer *buf,
                              struct smoothaf_persist_log_batch *batch);

/* Request flush of any pending data (non-blocking, sets flag) */
void smoothaf_log_fio_request_flush(struct smoothaf_log_fio_backend *fio);

/* Request clean close (non-blocking, sets flag) */
void smoothaf_log_fio_request_close(struct smoothaf_log_fio_backend *fio);

/* Check if backend has pending work */
bool smoothaf_log_fio_has_pending(const struct smoothaf_log_fio_backend *fio);
bool smoothaf_log_fio_is_quiescent(const struct smoothaf_log_fio_backend *fio);

/* Get error statistics */
uint32_t smoothaf_log_fio_get_write_errors(const struct smoothaf_log_fio_backend *fio);
uint32_t smoothaf_log_fio_get_partial_writes(const struct smoothaf_log_fio_backend *fio);
int smoothaf_log_fio_get_last_error(const struct smoothaf_log_fio_backend *fio);

/* Simulated card presence for host tests */
void smoothaf_log_fio_set_card_present(bool present);
bool smoothaf_log_fio_is_card_present(void);
#ifdef SMOOTHAF_HOST_TEST
/* Limit each simulated FIO write to exercise short-write retry handling. */
void smoothaf_log_fio_set_simulated_write_chunk(uint32_t max_bytes);
#endif

/* ============================================================
 * Internal Helpers (exposed for testing)
 * ============================================================ */

/* Get next numbered filename */
int smoothaf_log_fio_get_next_filename(struct smoothaf_log_fio_backend *fio, char *out, int maxlen);

/* Open new file, rotate if needed */
FILE *smoothaf_log_fio_open_file(struct smoothaf_log_fio_backend *fio);

/* Close current file */
void smoothaf_log_fio_close_file(struct smoothaf_log_fio_backend *fio);

/* Write batch to file */
smoothaf_persist_log_write_result_e smoothaf_log_fio_write_batch(struct smoothaf_log_fio_backend *fio,
                                                           const char *data, uint32_t len);

#endif /* SMOOTHAF_LOGGER_FIO_H */