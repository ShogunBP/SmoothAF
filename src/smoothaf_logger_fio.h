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
#include <stddef.h>
#include <stdio.h>

/* ============================================================
 * FIO Backend Configuration
 * ============================================================ */

#define SMOOTHAF_LOG_FIO_MAX_FILES      9        /* smoothaf000.log .. smoothaf008.log */
#define SMOOTHAF_LOG_FIO_MAX_SIZE_KB    256      /* Per-file size limit (KB) */
#define SMOOTHAF_LOG_FIO_DIR            "ML/LOGS" /* Log directory */
#define SMOOTHAF_LOG_FIO_PATTERN        "SMTH%03d.LOG"  /* 8.3 filename pattern */

/* Magic Lantern's fio-ml.h defines this as 0x80.  The FIO backend structure
 * crosses translation units, so the host fallback must have the same layout. */
#ifndef FIO_MAX_PATH_LENGTH
#define FIO_MAX_PATH_LENGTH 0x80
#endif
#if FIO_MAX_PATH_LENGTH != 0x80
#error "SmoothAF FIO backend requires FIO_MAX_PATH_LENGTH == 0x80"
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
    uint32_t file_open_attempts;           /* FIO_CreateFile attempts */
    uint32_t bytes_written;                /* Bytes accepted by FIO */
    uint32_t close_count;                  /* FIO_CloseFile calls */

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

/* ABI contract: this structure is shared by smoothaf.c and
 * smoothaf_logger_fio.c. Keep the checks in the shared header so every
 * translation unit fails at compile time if the Legacy layout changes. */
#define SMOOTHAF_LOG_FIO_FILENAME_OFFSET \
    (offsetof(struct smoothaf_log_fio_backend, current_filename))
#define SMOOTHAF_LOG_FIO_SIZE \
    (sizeof(struct smoothaf_log_fio_backend))

#if UINTPTR_MAX == 0xffffffffu
#define SMOOTHAF_LOG_FIO_EXPECTED_SIZE 0xe0u
#define SMOOTHAF_LOG_FIO_EXPECTED_FILENAME_OFFSET 0x10u
#define SMOOTHAF_LOG_FIO_EXPECTED_BYTES_WRITTEN_OFFSET 0xa0u
#define SMOOTHAF_LOG_FIO_EXPECTED_ENABLED_OFFSET 0xa8u
#define SMOOTHAF_LOG_FIO_EXPECTED_PENDING_OFFSET 0xaau
#define SMOOTHAF_LOG_FIO_EXPECTED_MAX_FILE_SIZE_OFFSET 0xacu
#else
#define SMOOTHAF_LOG_FIO_EXPECTED_SIZE 0xf0u
#define SMOOTHAF_LOG_FIO_EXPECTED_FILENAME_OFFSET 0x20u
#define SMOOTHAF_LOG_FIO_EXPECTED_BYTES_WRITTEN_OFFSET 0xb0u
#define SMOOTHAF_LOG_FIO_EXPECTED_ENABLED_OFFSET 0xb8u
#define SMOOTHAF_LOG_FIO_EXPECTED_PENDING_OFFSET 0xbau
#define SMOOTHAF_LOG_FIO_EXPECTED_MAX_FILE_SIZE_OFFSET 0xbcu
#endif

#define SMOOTHAF_LOG_FIO_STATIC_ASSERT(condition, name) \
    typedef char name[(condition) ? 1 : -1]

SMOOTHAF_LOG_FIO_STATIC_ASSERT(FIO_MAX_PATH_LENGTH == 0x80,
                               smoothaf_fio_path_length_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(SMOOTHAF_LOG_FIO_SIZE == SMOOTHAF_LOG_FIO_EXPECTED_SIZE,
                               smoothaf_fio_size_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(SMOOTHAF_LOG_FIO_FILENAME_OFFSET ==
                               SMOOTHAF_LOG_FIO_EXPECTED_FILENAME_OFFSET,
                               smoothaf_fio_filename_offset_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(offsetof(struct smoothaf_log_fio_backend, bytes_written) ==
                               SMOOTHAF_LOG_FIO_EXPECTED_BYTES_WRITTEN_OFFSET,
                               smoothaf_fio_bytes_offset_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(offsetof(struct smoothaf_log_fio_backend, enabled) ==
                               SMOOTHAF_LOG_FIO_EXPECTED_ENABLED_OFFSET,
                               smoothaf_fio_enabled_offset_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(offsetof(struct smoothaf_log_fio_backend, config_pending) ==
                               SMOOTHAF_LOG_FIO_EXPECTED_PENDING_OFFSET,
                               smoothaf_fio_pending_offset_contract);
SMOOTHAF_LOG_FIO_STATIC_ASSERT(offsetof(struct smoothaf_log_fio_backend, max_file_size_bytes) ==
                               SMOOTHAF_LOG_FIO_EXPECTED_MAX_FILE_SIZE_OFFSET,
                               smoothaf_fio_max_file_size_offset_contract);

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