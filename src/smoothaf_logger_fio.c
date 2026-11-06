/**
 * SmoothAF Logger — FIO Backend Implementation (Phase 3A)
 *
 * Uses ML FIO API: FIO_CreateFile, FIO_WriteFile, FIO_CloseFile,
 * get_numbered_file_name, __mem_malloc (via fio_malloc macro).
 *
 * No FIO calls in AF critical path. Consumer-only service loop.
 * 8.3 filenames, file rotation, error handling.
 * DISABLED BY DEFAULT.
 */

#include "smoothaf_logger_fio.h"
#include "smoothaf_logger.h"
#include <string.h>
#include <stdio.h>

#ifdef SMOOTHAF_HOST_TEST
#include <sys/stat.h>

/* Host-only stand-ins.  The ARM translation unit below includes the Legacy
 * declarations verbatim; these declarations are deliberately kept out of the
 * target build so they cannot silently become a second ABI. */
FILE *FIO_CreateFile(const char *name);
int FIO_WriteFile(FILE *stream, const void *ptr, size_t count);
void FIO_CloseFile(FILE *stream);
int get_numbered_file_name(const char *pattern, int nmax, char *filename, int maxlen);
int is_dir(const char *path);
#else
/* These are the canonical Legacy declarations.  In particular, fio-ml.h
 * supplies the FILE/FIO signatures and mem.h supplies __mem_free(void *),
 * not the old, incompatible file/line form. */
#include "fio-ml.h"
#include "mem.h"
#include "arm-mcr.h"
#endif

/* ============================================================
 * Host Simulation Stubs (for host tests)
 * ============================================================ */

#ifdef SMOOTHAF_HOST_TEST
/* Simulated file system for host tests */
static int sim_card_present = 1;
static uint32_t sim_write_chunk = 0;

static void sim_make_log_dir(void) {
    (void)mkdir("ML", 0777);
    (void)mkdir(SMOOTHAF_LOG_FIO_DIR, 0777);
}

static const char *sim_full_path(const char *name) {
    return name;
}

FILE* FIO_CreateFile(const char* name) {
    if (!sim_card_present || !name) return NULL;
    sim_make_log_dir();
    return fopen(sim_full_path(name), "wb");
}

int FIO_WriteFile(FILE* stream, const void* ptr, size_t count) {
    if (!stream) return -1;
    if (sim_write_chunk && count > sim_write_chunk) count = sim_write_chunk;
    return (int)fwrite(ptr, 1, count, stream);
}

void FIO_CloseFile(FILE* stream) {
    if (stream) fclose(stream);
}

int get_numbered_file_name(const char* pattern, int nmax, char* filename, int maxlen) {
    sim_make_log_dir();
    for (int num = 0; num <= nmax; num++) {
        snprintf(filename, maxlen, pattern, num);
        FILE *f = fopen(filename, "rb");
        if (!f) return num;
        fclose(f);
    }
    snprintf(filename, maxlen, pattern, 0);
    return -1;
}

void clean_d_cache(void) { /* no-op */ }

int is_dir(const char* path) {
    (void)path;
    return sim_card_present;
}

void smoothaf_log_fio_set_card_present(bool present) {
    sim_card_present = present ? 1 : 0;
}

void smoothaf_log_fio_set_simulated_write_chunk(uint32_t max_bytes) {
    sim_write_chunk = max_bytes;
}

bool smoothaf_log_fio_is_card_present(void) {
    return sim_card_present;
}

#else
/* Real ML environment - card presence check */
void smoothaf_log_fio_set_card_present(bool present) { (void)present; }
bool smoothaf_log_fio_is_card_present(void) {
    return is_dir("B:/") || is_dir("A:/");
}
#endif

/* ============================================================
 * FIO Backend Implementation
 * ============================================================ */

static uint32_t fio_retry_signature(const char *data, uint32_t len)
{
    uint32_t hash = 2166136261u;
    uint32_t i;
    for (i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

static smoothaf_persist_log_write_result_e fio_write_fn(const char *data, uint32_t len, void *ctx) {
    struct smoothaf_log_fio_backend *fio = (struct smoothaf_log_fio_backend *)ctx;
    return smoothaf_log_fio_write_batch(fio, data, len);
}

void smoothaf_log_fio_init(struct smoothaf_log_fio_backend *fio) {
    if (!fio) return;
    memset(fio, 0, sizeof(*fio));

    fio->base.write_fn = fio_write_fn;
    fio->base.ctx = fio;
    fio->base.max_batch_bytes = SMOOTHAF_LOG_BATCH_BUF_SIZE;

    fio->enabled = false;  /* DISABLED BY DEFAULT */
    fio->requested_enabled = false;
    fio->config_pending = false;
    fio->max_file_size_bytes = SMOOTHAF_LOG_FIO_MAX_SIZE_KB * 1024;
    fio->max_files = SMOOTHAF_LOG_FIO_MAX_FILES;
    fio->requested_max_file_size_bytes = fio->max_file_size_bytes;
    fio->requested_max_files = fio->max_files;
    fio->file_index = 0;
    fio->current_file = NULL;
    fio->current_file_size = 0;
    fio->total_files_written = 0;
    fio->file_open_attempts = 0;
    fio->bytes_written = 0;
    fio->close_count = 0;
    fio->write_errors = 0;
    fio->partial_writes = 0;
    fio->file_full_count = 0;
    fio->max_files_reached = 0;
    fio->last_error = 0;
    fio->flush_pending = false;
    fio->close_pending = false;
    fio->retry_pending = false;
    fio->retry_signature = 0;
    fio->retry_len = 0;
    fio->retry_offset = 0;
}

void smoothaf_log_fio_request_enabled(struct smoothaf_log_fio_backend *fio, bool enabled) {
    if (!fio) return;
    fio->requested_enabled = enabled;
    fio->config_pending = true;
}

void smoothaf_log_fio_request_limits(struct smoothaf_log_fio_backend *fio,
                                     uint32_t max_file_size_bytes,
                                     uint32_t max_files) {
    if (!fio) return;
    fio->requested_max_file_size_bytes = max_file_size_bytes;
    fio->requested_max_files = max_files;
    fio->config_pending = true;
}

static void fio_apply_enabled(struct smoothaf_log_fio_backend *fio, bool enabled)
{
    if (!fio) return;

    if (enabled && !fio->enabled) {
        /* Enabling: reset file index for new session */
        fio->file_index = 0;
        fio->total_files_written = 0;
        fio->current_file_size = 0;
        if (fio->current_file) {
            FIO_CloseFile(fio->current_file);
            fio->close_count++;
            fio->current_file = NULL;
        }
        fio->retry_pending = false;
        fio->retry_signature = 0;
        fio->retry_len = 0;
        fio->retry_offset = 0;
    } else if (!enabled && fio->enabled) {
        /* Disabling: close any open file */
        if (fio->current_file) {
            FIO_CloseFile(fio->current_file);
            fio->close_count++;
            fio->current_file = NULL;
        }
        fio->retry_pending = false;
        fio->retry_signature = 0;
        fio->retry_len = 0;
        fio->retry_offset = 0;
    }
    fio->enabled = enabled;
    fio->config_pending = false;
}

void smoothaf_log_fio_set_enabled(struct smoothaf_log_fio_backend *fio, bool enabled)
{
    if (!fio) return;
    fio->requested_enabled = enabled;
    fio_apply_enabled(fio, enabled);
    fio->requested_enabled = enabled;
}

bool smoothaf_log_fio_is_enabled(const struct smoothaf_log_fio_backend *fio) {
    return fio ? fio->enabled : false;
}

bool smoothaf_log_fio_has_pending(const struct smoothaf_log_fio_backend *fio) {
    return fio && (fio->flush_pending || fio->close_pending || fio->current_file || fio->config_pending);
}

bool smoothaf_log_fio_is_quiescent(const struct smoothaf_log_fio_backend *fio) {
    return fio && !fio->enabled && !fio->current_file &&
           (!fio->config_pending || !fio->requested_enabled);
}

void smoothaf_log_fio_request_flush(struct smoothaf_log_fio_backend *fio) {
    if (fio && fio->enabled) fio->flush_pending = true;
}

void smoothaf_log_fio_request_close(struct smoothaf_log_fio_backend *fio) {
    if (fio) fio->close_pending = true;
}

/* Get next available filename using ML's get_numbered_file_name */
int smoothaf_log_fio_get_next_filename(struct smoothaf_log_fio_backend *fio, char *out, int maxlen) {
    if (!fio || !out) return -1;

    /* Probe the same directory in which files are created. Use the ML helper
     * so existing SMTH000.LOG..SMTH008.LOG files are never overwritten. */
    char pattern[FIO_MAX_PATH_LENGTH];
    size_t dir_len = strlen(SMOOTHAF_LOG_FIO_DIR);
    size_t pattern_len = strlen(SMOOTHAF_LOG_FIO_PATTERN);
    if (dir_len + 1 + pattern_len + 1 > sizeof(pattern)) return -1;
    memcpy(pattern, SMOOTHAF_LOG_FIO_DIR, dir_len);
    pattern[dir_len] = '/';
    memcpy(pattern + dir_len + 1, SMOOTHAF_LOG_FIO_PATTERN, pattern_len + 1);

    int result = get_numbered_file_name(pattern,
                                         (int)fio->max_files - 1,
                                         out, maxlen);
    if (result >= 0) {
        fio->file_index = (uint32_t)result;
    }
    return result;
}

/* Open new log file, creating directory if needed */
FILE *smoothaf_log_fio_open_file(struct smoothaf_log_fio_backend *fio) {
    if (!fio || !fio->enabled) return NULL;

    /* Check card presence */
    if (!smoothaf_log_fio_is_card_present()) {
        fio->last_error = -1;  /* No card */
        return NULL;
    }

    /* Get next filename */
    char filename[FIO_MAX_PATH_LENGTH];
    if (smoothaf_log_fio_get_next_filename(fio, filename, sizeof(filename)) < 0) {
        fio->last_error = -2;  /* Max files reached */
        fio->max_files_reached++;
        return NULL;
    }

    /* Create file (ML wrapper creates directories). get_numbered_file_name
     * returned the complete ML/LOGS/SMTHxxx.LOG path. */
    fio->file_open_attempts++;
    FILE *f = FIO_CreateFile(filename);
    if (!f) {
        fio->last_error = -3;  /* Create failed */
        fio->write_errors++;
        return NULL;
    }

    /* Save filename and reset size */
    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;
    size_t basename_len = strlen(basename);
    if (basename_len >= sizeof(fio->current_filename))
        basename_len = sizeof(fio->current_filename) - 1;
    memcpy(fio->current_filename, basename, basename_len);
    fio->current_filename[basename_len] = '\0';
    fio->current_file_size = 0;
    fio->total_files_written++;

    return f;
}

/* Close current file */
void smoothaf_log_fio_close_file(struct smoothaf_log_fio_backend *fio) {
    if (!fio || !fio->current_file) return;

    FIO_CloseFile(fio->current_file);
    fio->close_count++;
    fio->current_file = NULL;
    fio->current_file_size = 0;
    fio->current_filename[0] = '\0';
}

/* Write batch to file with rotation and error handling */
smoothaf_persist_log_write_result_e smoothaf_log_fio_write_batch(struct smoothaf_log_fio_backend *fio,
                                                           const char *data, uint32_t len) {
    if (!fio || !fio->enabled) return SMOOTHAF_PERSIST_LOG_WRITE_OK;  /* No-op when disabled */
    if (!data || len == 0) return SMOOTHAF_PERSIST_LOG_WRITE_OK;

    /* Check card presence */
    if (!smoothaf_log_fio_is_card_present()) {
        fio->last_error = -1;
        fio->write_errors++;
        return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
    }

    /* Open file if needed */
    if (!fio->current_file) {
        fio->current_file = smoothaf_log_fio_open_file(fio);
        if (!fio->current_file) {
            return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
        }
    }

    /* A retry resumes the same line in the same file. Never rotate an
     * in-progress line based on the original line length. */
    uint32_t offset;
    uint32_t signature = fio_retry_signature(data, len);
    bool same_retry = fio->retry_pending && fio->retry_signature == signature &&
                      fio->retry_len == len && fio->retry_offset <= len;
    if (same_retry)
        offset = fio->retry_offset;
    else {
        fio->retry_pending = false;
        fio->retry_signature = signature;
        fio->retry_len = len;
        fio->retry_offset = 0;
        offset = 0;
    }

    /* Keep every individual write within the configured file limit. The
     * logger's normal batch is bounded to 4 KB, but tests/configuration may
     * use a smaller limit. */
    if (fio->max_file_size_bytes == 0 || len > fio->max_file_size_bytes) {
        fio->last_error = -6;  /* Batch cannot fit in one file */
        fio->file_full_count++;
        return SMOOTHAF_PERSIST_LOG_WRITE_FULL;
    }

    /* Check file size limit before a new line. A retry is already committed to
     * this file and must complete there, even if the remaining suffix is small. */
    if (!same_retry && fio->current_file_size > fio->max_file_size_bytes - len) {
        smoothaf_log_fio_close_file(fio);
        fio->current_file = smoothaf_log_fio_open_file(fio);
        if (!fio->current_file) {
            fio->file_full_count++;
            return SMOOTHAF_PERSIST_LOG_WRITE_FULL;
        }
        fio->file_full_count++;
    }

    /* Ensure cache is clean for DMA */
    #ifndef SMOOTHAF_HOST_TEST
    clean_d_cache();
    #endif

    /* Write data. FIO_WriteFile returns the number of bytes written, so retry
     * a short write from the unwritten suffix while the card/file remains
     * usable. */
    while (offset < len) {
        int written = FIO_WriteFile(fio->current_file, data + offset, len - offset);
        if (written < 0) {
            fio->last_error = -4;  /* Write failed */
            fio->write_errors++;
            fio->retry_pending = true;
            return SMOOTHAF_PERSIST_LOG_WRITE_ERR;
        }
        if (written == 0) {
            fio->last_error = -5;  /* No progress / partial write */
            fio->partial_writes++;
            fio->retry_pending = true;
            return SMOOTHAF_PERSIST_LOG_WRITE_PARTIAL;
        }

        offset += (uint32_t)written;
        fio->retry_offset = offset;
        fio->current_file_size += (uint32_t)written;
        fio->bytes_written += (uint32_t)written;
        if (offset < len) {
            fio->last_error = -5;
            fio->partial_writes++;
        }
    }

    fio->last_error = 0;
    fio->retry_pending = false;
    fio->retry_signature = 0;
    fio->retry_len = 0;
    fio->retry_offset = 0;
    return SMOOTHAF_PERSIST_LOG_WRITE_OK;
}

/* Consumer service loop - call periodically from non-critical task */
bool smoothaf_log_fio_service(struct smoothaf_log_fio_backend *fio,
                              struct smoothaf_persist_log_buffer *buf,
                              struct smoothaf_persist_log_batch *batch) {
    if (!fio || !buf || !batch) return false;

    bool work_done = false;

    /* A failed batch is a durable consumer-side retry obligation. */
    if (buf->consumer_pending) fio->flush_pending = true;

    if (fio->config_pending) {
        bool requested = fio->requested_enabled;
        uint32_t requested_size = fio->requested_max_file_size_bytes;
        uint32_t requested_files = fio->requested_max_files;
        fio->config_pending = false;
        if (requested_size > 0) fio->max_file_size_bytes = requested_size;
        if (requested_files > 0 && requested_files <= SMOOTHAF_LOG_FIO_MAX_FILES)
            fio->max_files = requested_files;
        fio_apply_enabled(fio, requested);
        /* A publisher may have changed the request while it was being
         * applied. Do not acknowledge that newer request accidentally. */
        if (fio->requested_enabled != requested ||
            fio->requested_max_file_size_bytes != requested_size ||
            fio->requested_max_files != requested_files)
            fio->config_pending = true;
        work_done = true;
    }

    if (!fio->enabled) {
        fio->close_pending = false;
        fio->flush_pending = false;
        return work_done;
    }

    /* Consume and flush one bounded batch. A failed flush leaves the current
     * batch retryable and the logger tail at the first unwritten record. */
    if (fio->enabled && (fio->flush_pending || fio->close_pending ||
                         !smoothaf_persist_log_is_empty(buf) ||
                         buf->consumer_pending)) {
        uint32_t consumed = 0;
        if (!buf->consumer_pending)
            consumed = smoothaf_persist_log_consume_batch(buf, batch);
        else
            consumed = batch->count - batch->next_line;
        if (consumed > 0) {
            smoothaf_persist_log_write_result_e result =
                smoothaf_persist_log_flush_batch(buf, batch, &fio->base);
            work_done = true;
            if (result != SMOOTHAF_PERSIST_LOG_WRITE_OK) {
                fio->last_error = (result == SMOOTHAF_PERSIST_LOG_WRITE_PARTIAL) ? -5 : fio->last_error;
                /* Keep the request pending: the batch and ring reservation are
                 * still retryable, and a later consumer pass must retry it. */
                fio->flush_pending = true;
                return work_done;
            }
        }
        fio->flush_pending = false;
    }

    if (fio->close_pending) {
        if (smoothaf_persist_log_is_empty(buf) && !buf->consumer_pending) {
            smoothaf_log_fio_close_file(fio);
            fio->close_pending = false;
            smoothaf_log_fio_set_enabled(fio, false);
        }
        return true;
    }

    return work_done;
}

/* Statistics */
uint32_t smoothaf_log_fio_get_write_errors(const struct smoothaf_log_fio_backend *fio) {
    return fio ? fio->write_errors : 0;
}

uint32_t smoothaf_log_fio_get_partial_writes(const struct smoothaf_log_fio_backend *fio) {
    return fio ? fio->partial_writes : 0;
}

int smoothaf_log_fio_get_last_error(const struct smoothaf_log_fio_backend *fio) {
    return fio ? fio->last_error : 0;
}