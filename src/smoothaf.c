/*
 * SmoothAF 0.2.0 - bounded single-shot contrast focus for Magic Lantern (Legacy).
 *
 * Target: Canon EOS 600D / Rebel T3i, firmware 1.0.2, DIGIC IV,
 *         Magic Lantern Legacy Lua Fix, EF 50mm f/1.8 STM.
 *
 * Design rules for this version:
 *   - no continuous AF, Movie AF, RAW AF, face detection or auto shooting;
 *   - one SmoothAF task at a time, reserved before task_create();
 *   - one synchronous lens_focus(wait=1) command at a time;
 *   - Stop only prevents future commands; a command in flight may finish;
 *   - all sharpness logic is integer-only and allocation-free per frame;
 *   - no lens movement while the module is loading.
 *
 * The hardware-independent logic lives in smoothaf_core.h and is covered by
 * modules/smoothaf/tests/test_smoothaf_core.c (host tests, not a module build).
 */

#include <stdint.h>
#include <module.h>
#include <dryos.h>
#include <config.h>
#include <menu.h>
#include <lens.h>
#include <focus.h>
#include <vram.h>
#include <propvalues.h>

#include "smoothaf_core.h"
#include "smoothaf_monitor.h"
#include "smoothaf_diagnostics.h"
#include "smoothaf_af_assist.h"

#ifndef SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
#define SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS 0
#endif

#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
#include "smoothaf_log_export.h"
#include "smoothaf_fio_test.h"
#include <fio-ml.h>
#endif

#define SMOOTHAF_HC_LEGACY       0
#define SMOOTHAF_HC_ADAPTIVE     1
#define SMOOTHAF_AF_NORMAL       2

/* Experimental timing values for the EF 50mm f/1.8 STM.  They have not been
 * calibrated on hardware; lens_focus(wait=1) itself can block for up to about
 * two seconds (property wait + focus confirmation wait). */
#define SMOOTHAF_SETTLE_MS        120
#define SMOOTHAF_COOLDOWN_MS      80
#define SMOOTHAF_TIMEOUT_MS       5000
#define SMOOTHAF_VLOG_AF_MAX_COMMANDS 4
#define SMOOTHAF_VLOG_AF_MAX_REVERSALS 1

#define SMOOTHAF_TASK_PRIORITY    0x1c
#define SMOOTHAF_TASK_STACK       0x1000
#define SMOOTHAF_DEINIT_WAIT_MS   3000

#define SMOOTHAF_TASK_MEASURE     1
#define SMOOTHAF_TASK_STEP        2
#define SMOOTHAF_TASK_SEARCH      3
#define SMOOTHAF_TASK_MONITOR     4

#define SMOOTHAF_STATUS_IDLE          0
#define SMOOTHAF_STATUS_MEASURING     1
#define SMOOTHAF_STATUS_MOVING        2
#define SMOOTHAF_STATUS_LOCKED        3
#define SMOOTHAF_STATUS_SEARCH_LIMIT  4
#define SMOOTHAF_STATUS_NO_CONFIDENCE 5
#define SMOOTHAF_STATUS_CANCELLED     6
#define SMOOTHAF_STATUS_TIMEOUT       7
#define SMOOTHAF_STATUS_LENS_ERROR    8
#define SMOOTHAF_STATUS_ERROR         9

#define SMOOTHAF_BLOCK_NONE           0
#define SMOOTHAF_BLOCK_DISABLED       1
#define SMOOTHAF_BLOCK_CANCELLED      2
#define SMOOTHAF_BLOCK_NO_LV          3
#define SMOOTHAF_BLOCK_MOVIE          4
#define SMOOTHAF_BLOCK_RECORDING      5
#define SMOOTHAF_BLOCK_MANUAL_FOCUS   6
#define SMOOTHAF_BLOCK_CONTINUOUS_AF  7
#define SMOOTHAF_BLOCK_FOLLOW_FOCUS   8
#define SMOOTHAF_BLOCK_RACK_FOCUS     9
#define SMOOTHAF_BLOCK_FOCUS_STACK    10
#define SMOOTHAF_BLOCK_PARAMS         11
#define SMOOTHAF_BLOCK_SEMAPHORE      12
#define SMOOTHAF_BLOCK_TASK_CREATE   13
#define SMOOTHAF_BLOCK_FRAME         14
#define SMOOTHAF_BLOCK_GEOMETRY      15

/* Persisted settings.  Enable is deliberately NOT persisted: it must always
 * start disabled, even after a module reload. */
static CONFIG_INT("smoothaf.metric", smoothaf_metric, SMOOTHAF_METRIC_SAD);
static CONFIG_INT("smoothaf.step", smoothaf_step, 1);
static CONFIG_INT("smoothaf.dir", smoothaf_direction_choice, 0); /* 0=A, 1=B */
static CONFIG_INT("smoothaf.hc_mode", smoothaf_hc_mode, SMOOTHAF_HC_LEGACY);
static CONFIG_INT("smoothaf.af_mode", smoothaf_af_mode, -1);
static CONFIG_INT("smoothaf.debug", smoothaf_debug_logging, 0);
static CONFIG_INT("smoothaf.monitor_interval", smoothaf_monitor_interval, SMOOTHAF_MONITOR_INTERVAL_DEFAULT);

static int smoothaf_enabled = 0;
static int smoothaf_vlog_af_mode = SMOOTHAF_VLOG_AF_OFF;
static struct smoothaf_log_buffer smoothaf_log;
static struct smoothaf_af_log_buffer smoothaf_af_log;
static struct smoothaf_af_assist smoothaf_af_assist;

static volatile int smoothaf_status = SMOOTHAF_STATUS_IDLE;
static volatile int smoothaf_cancel = 0;
static volatile int smoothaf_task_running = 0;
static volatile int smoothaf_task_stop_requested = 0;
static volatile int smoothaf_task_exited = 1;

static volatile int smoothaf_last_score = 0;
static volatile int smoothaf_last_sad = 0;
static volatile int smoothaf_last_laplacian = 0;
static volatile int smoothaf_last_measure_valid = 0;
static volatile int smoothaf_last_best = 0;
static volatile int smoothaf_last_mean = 0;
static volatile int smoothaf_last_time = 0;
static volatile int smoothaf_last_focus_pos = 0;
static volatile int smoothaf_last_error = 0;
static volatile int smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
static volatile int smoothaf_last_commands = 0;
static volatile int smoothaf_last_reversals = 0;
static volatile int smoothaf_last_lens_called = 0;
static volatile int smoothaf_last_lens_return = 0;
static volatile int smoothaf_af_move_attempts = 0;
static volatile int smoothaf_af_lens_calls = 0;
static volatile int smoothaf_af_move_successes = 0;
static volatile int smoothaf_af_commands_episode = 0;
static volatile int smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
static volatile int smoothaf_af_initial_score = 0;
static volatile int smoothaf_af_post_move_score = 0;
static volatile int smoothaf_af_best_score = 0;
static volatile int smoothaf_af_score_delta = 0;
static volatile int smoothaf_af_best_position = 0;
static volatile int smoothaf_af_current_position = 0;
static volatile int smoothaf_af_last_direction = 1;
static volatile int smoothaf_last_mode = SMOOTHAF_HC_LEGACY;
static volatile int smoothaf_last_phase = 0;

static struct smoothaf_monitor smoothaf_monitor;
static struct smoothaf_vlog_af smoothaf_vlog_af;
static volatile int smoothaf_monitor_task_requested = 0;
static volatile int smoothaf_monitor_deinit = 0;
static volatile int smoothaf_log_publish_failures = 0;
static volatile int smoothaf_last_measure_code = 0;
#define SMOOTHAF_MEASURE_OK       0
#define SMOOTHAF_MEASURE_LOW      1
#define SMOOTHAF_MEASURE_ERROR    2
#define SMOOTHAF_MEASURE_WAITING  3
static struct smoothaf_log_entry smoothaf_diag_snapshot[SMOOTHAF_DIAG_CAPACITY];
static struct smoothaf_log_entry smoothaf_af_export_snapshot[SMOOTHAF_AF_DIAG_CAPACITY];
static size_t smoothaf_diag_count = 0;
static size_t smoothaf_diag_page = 0;
static size_t smoothaf_diag_seen_count = 0;
static uint32_t smoothaf_session_started_ms = 0;
static uint32_t smoothaf_session_finished_ms = 0;
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
static char smoothaf_last_export_path[128];
static int smoothaf_last_export_error = 0;
static int smoothaf_last_export_stage = 0;
static int smoothaf_last_export_raw_return = 0;
static int smoothaf_last_export_write_return = 0;
static int smoothaf_sd_test_stage = 0;
static int smoothaf_sd_test_raw_return = 0;
static int smoothaf_sd_test_collision = 0;
static int smoothaf_sd_test_success = 0;
#endif
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
static struct smoothaf_fio_state smoothaf_fio_create_state;
static struct smoothaf_fio_state smoothaf_fio_edit_state;
static struct smoothaf_fio_state smoothaf_fio_paths_state;
static struct smoothaf_fio_path_state smoothaf_fio_path_results[7];
static int smoothaf_fio_path_entry = 0;
static int smoothaf_fio_shutdown = 0;
#endif

#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
#define SMOOTHAF_SD_TEST_STAGE_READY       0
#define SMOOTHAF_SD_TEST_STAGE_TASK_GATE   1
#define SMOOTHAF_SD_TEST_STAGE_DIRECTORY   2
#define SMOOTHAF_SD_TEST_STAGE_NAME_CHECK  3
#define SMOOTHAF_SD_TEST_STAGE_FILE_CREATE 4
#define SMOOTHAF_SD_TEST_STAGE_WRITE       5
#define SMOOTHAF_SD_TEST_STAGE_CLOSE       6
#define SMOOTHAF_SD_TEST_STAGE_SUCCESS     7

#define SMOOTHAF_SD_TEST_PATH "ML/LOGS/SMOOTHAF-SD-TEST.LOG"
#define SMOOTHAF_SD_TEST_CONTENT "SmoothAF SD WRITE TEST 1\n"

#define SMOOTHAF_EXPORT_STAGE_READY       0
#define SMOOTHAF_EXPORT_STAGE_TASK_GATE   1
#define SMOOTHAF_EXPORT_STAGE_SNAPSHOT    2
#define SMOOTHAF_EXPORT_STAGE_DIRECTORY   3
#define SMOOTHAF_EXPORT_STAGE_NAME_CHECK  4
#define SMOOTHAF_EXPORT_STAGE_FILE_CREATE 5
#define SMOOTHAF_EXPORT_STAGE_WRITE       6
#define SMOOTHAF_EXPORT_STAGE_CLOSE       7
#define SMOOTHAF_EXPORT_STAGE_SUCCESS     8

static const char *smoothaf_export_stage_name(int stage)
{
    switch (stage)
    {
        case SMOOTHAF_EXPORT_STAGE_TASK_GATE:   return "TASK_GATE";
        case SMOOTHAF_EXPORT_STAGE_SNAPSHOT:    return "SNAPSHOT";
        case SMOOTHAF_EXPORT_STAGE_DIRECTORY:   return "DIRECTORY";
        case SMOOTHAF_EXPORT_STAGE_NAME_CHECK:  return "NAME_CHECK";
        case SMOOTHAF_EXPORT_STAGE_FILE_CREATE: return "FILE_CREATE";
        case SMOOTHAF_EXPORT_STAGE_WRITE:       return "WRITE";
        case SMOOTHAF_EXPORT_STAGE_CLOSE:       return "CLOSE";
        case SMOOTHAF_EXPORT_STAGE_SUCCESS:     return "SUCCESS";
        default:                                return "READY";
    }
}

static const char *smoothaf_sd_test_stage_name(int stage)
{
    switch (stage)
    {
        case SMOOTHAF_SD_TEST_STAGE_TASK_GATE:   return "TASK_GATE";
        case SMOOTHAF_SD_TEST_STAGE_DIRECTORY:   return "DIRECTORY";
        case SMOOTHAF_SD_TEST_STAGE_NAME_CHECK:  return "NAME_CHECK";
        case SMOOTHAF_SD_TEST_STAGE_FILE_CREATE: return "FILE_CREATE";
        case SMOOTHAF_SD_TEST_STAGE_WRITE:       return "WRITE";
        case SMOOTHAF_SD_TEST_STAGE_CLOSE:       return "CLOSE";
        case SMOOTHAF_SD_TEST_STAGE_SUCCESS:     return "SUCCESS";
        default:                                 return "READY";
    }
}

static struct semaphore *smoothaf_sem = 0;

static void smoothaf_log_event_extended(smoothaf_log_type_t type,
                                        int32_t value1, int32_t value2,
                                        int32_t value3, int32_t value4,
                                        int32_t value5);
static int smoothaf_is_task_running(void);

static const char *smoothaf_monitor_state_name(void)
{
    switch (smoothaf_monitor.state)
    {
        case SMOOTHAF_MONITOR_MONITORING:    return "MONITORING";
        case SMOOTHAF_MONITOR_POSSIBLE_BLUR: return "POSSIBLE_BLUR";
        case SMOOTHAF_MONITOR_UNCERTAIN:     return "UNCERTAIN";
        case SMOOTHAF_MONITOR_STABLE:        return "STABLE";
        case SMOOTHAF_MONITOR_PAUSED:        return "PAUSED";
        case SMOOTHAF_MONITOR_ERROR:         return "ERROR";
        default:                             return "IDLE";
    }
}

static void smoothaf_monitor_log_transitions(void)
{
    int from;
    int to;
    if (smoothaf_monitor_take_transition(&smoothaf_monitor, &from, &to))
    {
        smoothaf_log_event_extended(SMOOTHAF_LOG_MONITOR, from, to,
                                    smoothaf_monitor.reference,
                                    smoothaf_monitor.smoothed,
                                    smoothaf_monitor.metrics.valid);
    }
}

static void smoothaf_monitor_update_from_measurement(int score, int mean,
                                                       int sad, int laplacian,
                                                       int valid, int code,
                                                       uint32_t duration)
{
    struct smoothaf_monitor_sample sample;
    if (!smoothaf_monitor.enabled)
        return;

    sample.score = score;
    sample.mean = mean;
    sample.sad = sad;
    sample.laplacian = laplacian;
    sample.valid = valid;
    sample.confident = valid && smoothaf_has_confidence(score, mean);
    sample.metric = smoothaf_metric == SMOOTHAF_METRIC_LAPLACIAN
                  ? SMOOTHAF_MONITOR_METRIC_LAPLACIAN
                  : SMOOTHAF_MONITOR_METRIC_SAD;
    sample.duration_ms = duration;
    (void)code;
    smoothaf_monitor_accept(&smoothaf_monitor, &sample,
                            (uint32_t)get_ms_clock());
    smoothaf_monitor_log_transitions();
}

static void smoothaf_publish_measurement(int score, int mean, int sad,
                                         int laplacian, int valid, int code)
{
    smoothaf_last_measure_code = code;
    smoothaf_last_mean = mean;
    smoothaf_last_sad = sad;
    smoothaf_last_laplacian = laplacian;
    smoothaf_last_measure_valid = valid ? 1 : 0;
    smoothaf_last_error = valid ? 0 : 1;

    if (!valid)
    {
        smoothaf_last_score = -1;
        smoothaf_monitor_update_from_measurement(-1, mean, sad, laplacian,
                                                 0, code, 0);
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR, code, mean, sad,
                                    laplacian, 0);
        return;
    }

    smoothaf_last_score = score;
    smoothaf_last_best = score;
    smoothaf_monitor_update_from_measurement(score, mean, sad, laplacian,
                                             1, code, 0);
    smoothaf_log_event_extended(SMOOTHAF_LOG_MEASURE, score, mean, sad,
                                laplacian, code);
}

static const char *smoothaf_status_name(void)
{
    switch (smoothaf_status)
    {
        case SMOOTHAF_STATUS_MEASURING:     return "Measuring";
        case SMOOTHAF_STATUS_MOVING:        return "Moving";
        case SMOOTHAF_STATUS_LOCKED:        return "Focus Locked";
        case SMOOTHAF_STATUS_SEARCH_LIMIT:  return "Search Limit";
        case SMOOTHAF_STATUS_NO_CONFIDENCE: return "No Confidence";
        case SMOOTHAF_STATUS_CANCELLED:     return "Cancelled";
        case SMOOTHAF_STATUS_TIMEOUT:       return "Timeout";
        case SMOOTHAF_STATUS_LENS_ERROR:    return "Lens Error";
        case SMOOTHAF_STATUS_ERROR:         return "Error";
        default:                          return "Idle";
    }
}

static const char *smoothaf_block_name(int reason)
{
    switch (reason)
    {
        case SMOOTHAF_BLOCK_DISABLED:      return "Disabled";
        case SMOOTHAF_BLOCK_CANCELLED:     return "Stopped";
        case SMOOTHAF_BLOCK_NO_LV:         return "No LiveView";
        case SMOOTHAF_BLOCK_MOVIE:         return "Movie mode";
        case SMOOTHAF_BLOCK_RECORDING:     return "Recording";
        case SMOOTHAF_BLOCK_MANUAL_FOCUS:  return "Manual focus";
        case SMOOTHAF_BLOCK_CONTINUOUS_AF: return "Continuous AF";
        case SMOOTHAF_BLOCK_FOLLOW_FOCUS:  return "Follow Focus";
        case SMOOTHAF_BLOCK_RACK_FOCUS:    return "Rack Focus";
        case SMOOTHAF_BLOCK_FOCUS_STACK:   return "Focus Stacking";
        case SMOOTHAF_BLOCK_PARAMS:        return "Invalid parameters";
        case SMOOTHAF_BLOCK_SEMAPHORE:     return "Semaphore unavailable";
        case SMOOTHAF_BLOCK_TASK_CREATE:   return "Task creation failed";
        case SMOOTHAF_BLOCK_FRAME:         return "Invalid frame";
        case SMOOTHAF_BLOCK_GEOMETRY:      return "Invalid geometry";
        default:                         return "None";
    }
}

/* Control-state lock.  volatile alone is not used as mutual exclusion. */
static int smoothaf_lock(void)
{
    return smoothaf_sem && take_semaphore(smoothaf_sem, 50) == 0;
}

static void smoothaf_unlock(void)
{
    if (smoothaf_sem)
    {
        give_semaphore(smoothaf_sem);
    }
}

static int smoothaf_direction_value(void)
{
    return smoothaf_direction_choice ? 1 : -1;
}

static void smoothaf_log_event_extended(smoothaf_log_type_t type,
                                        int32_t value1, int32_t value2,
                                        int32_t value3, int32_t value4,
                                        int32_t value5)
{
    if (!smoothaf_debug_logging)
    {
        return;
    }
    if (!smoothaf_lock())
    {
        smoothaf_log_publish_failures++;
        return;
    }

    smoothaf_log_set_enabled(&smoothaf_log, 1);
    smoothaf_log.tick = (uint32_t)get_ms_clock();
    smoothaf_log_write_extended(&smoothaf_log, type, value1, value2, value3,
                                value4, value5);
    if (type == SMOOTHAF_LOG_MOVE || type == SMOOTHAF_LOG_MEASURE ||
        type == SMOOTHAF_LOG_CONTROLLER || type == SMOOTHAF_LOG_ABORT ||
        type == SMOOTHAF_LOG_ERROR || type == SMOOTHAF_LOG_TASK_END)
    {
        smoothaf_af_log_write(&smoothaf_af_log, smoothaf_log.tick, type,
                              value1, value2, value3, value4, value5);
    }
    smoothaf_unlock();
}

static void smoothaf_log_event(smoothaf_log_type_t type,
                               int32_t value1, int32_t value2, int32_t value3)
{
    smoothaf_log_event_extended(type, value1, value2, value3, 0, 0);
}

#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
struct smoothaf_file_writer
{
    FILE *file;
    int raw_return;
};

static int smoothaf_file_write(void *context, const char *data, size_t length)
{
    struct smoothaf_file_writer *writer = context;
    writer->raw_return = FIO_WriteFile(writer->file, data, length);
    return writer->raw_return == (int)length ? writer->raw_return : -1;
}

static const char *smoothaf_log_mode_name(void)
{
    switch (smoothaf_vlog_af_mode)
    {
        case SMOOTHAF_VLOG_AF_DRY_RUN: return "DRY RUN";
        case SMOOTHAF_VLOG_AF_ACTIVE:  return "ACTIVE";
        default:                       return "OFF";
    }
}

static int smoothaf_export_af_log(void)
{
    struct smoothaf_log_entry *snapshot = smoothaf_af_export_snapshot;
    struct smoothaf_log_export_metadata metadata;
    struct smoothaf_log_export_result result;
    struct smoothaf_file_writer writer;
    FILE *file = 0;
    char path[FIO_MAX_PATH_LENGTH];
    int number;
    int count;
    int rc;

    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_TASK_GATE;
    smoothaf_last_export_raw_return = 0;
    smoothaf_last_export_write_return = 0;
    smoothaf_last_export_path[0] = '\0';
    if (smoothaf_is_task_running() || !smoothaf_task_exited)
    {
        smoothaf_last_export_error = 1;
        return -1;
    }
    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_SNAPSHOT;
    if (!smoothaf_lock())
    {
        smoothaf_last_export_error = 2;
        return -2;
    }
    count = (int)smoothaf_af_log_snapshot(&smoothaf_af_log, snapshot,
                                          SMOOTHAF_AF_DIAG_CAPACITY);
    metadata.mode = smoothaf_log_mode_name();
    metadata.configured_interval_ms = smoothaf_monitor_interval_ms(
        smoothaf_monitor.interval_index);
    metadata.observed_interval_ms = smoothaf_monitor.metrics.actual_interval_ms;
    metadata.session_started_ms = smoothaf_session_started_ms;
    metadata.session_finished_ms = smoothaf_session_finished_ms;
    metadata.dropped = smoothaf_log.dropped;
    smoothaf_unlock();

    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_DIRECTORY;
    if (!is_dir("ML/LOGS"))
    {
        smoothaf_last_export_raw_return = FIO_CreateDirectory("ML/LOGS");
        if (smoothaf_last_export_raw_return != 0)
        {
            smoothaf_last_export_error = 3;
            return -3;
        }
    }
    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_NAME_CHECK;
    for (number = 0; number <= 999; number++)
    {
        snprintf(path, sizeof(path), "ML/LOGS/SMOOTHAF-%03d.LOG", number);
        if (!is_file(path))
            break;
    }
    if (number > 999)
    {
        smoothaf_last_export_error = 4;
        return -4;
    }
    snprintf(smoothaf_last_export_path, sizeof(smoothaf_last_export_path),
             "%s", path);

    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_FILE_CREATE;
    file = FIO_CreateFile(path);
    if (!file)
    {
        smoothaf_last_export_error = 5;
        return -5;
    }
    writer.file = file;
    writer.raw_return = 0;
    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_WRITE;
    rc = smoothaf_log_export_snapshot(snapshot, (size_t)count, &metadata,
                                      smoothaf_file_write, &writer, &result);
    smoothaf_last_export_write_return = writer.raw_return;
    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_CLOSE;
    FIO_CloseFile(file);
    if (rc)
    {
        smoothaf_last_export_raw_return = rc;
        smoothaf_last_export_error = 6;
        return -6;
    }
    smoothaf_last_export_stage = SMOOTHAF_EXPORT_STAGE_SUCCESS;
    smoothaf_last_export_error = 0;
    return 0;
}

static MENU_UPDATE_FUNC(smoothaf_export_log_update)
{
    if (smoothaf_last_export_error)
    {
        if (smoothaf_last_export_stage == SMOOTHAF_EXPORT_STAGE_FILE_CREATE)
        {
            MENU_SET_VALUE("ERROR %d %s handle=NULL", smoothaf_last_export_error,
                           smoothaf_export_stage_name(smoothaf_last_export_stage));
        }
        else
        {
            MENU_SET_VALUE("ERROR %d %s raw=%d", smoothaf_last_export_error,
                           smoothaf_export_stage_name(smoothaf_last_export_stage),
                           smoothaf_last_export_stage == SMOOTHAF_EXPORT_STAGE_WRITE
                           ? smoothaf_last_export_write_return
                           : smoothaf_last_export_raw_return);
        }
    }
    else if (smoothaf_last_export_path[0])
        MENU_SET_VALUE("%s", smoothaf_last_export_path);
    else
        MENU_SET_VALUE("READY");
}

static MENU_SELECT_FUNC(smoothaf_export_log_select)
{
    (void)smoothaf_export_af_log();
}
#endif

#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
#define SMOOTHAF_FIO_ROOT_CREATE_PATH "SMOOTHAF-ROOT-CREATE.LOG"
#define SMOOTHAF_FIO_ROOT_EDIT_PATH "SMOOTHAF-ROOT-TEST.LOG"
#define SMOOTHAF_FIO_ROOT_CREATE_CONTENT "SMOOTHAF ROOT CREATE OK"
#define SMOOTHAF_FIO_ROOT_EDIT_CONTENT "SMOOTHAF ROOT EDIT OK"
#define SMOOTHAF_FIO_ROOT_EDIT_SIZE 4096
#define SMOOTHAF_FIO_PATH_COUNT 7

static const char *smoothaf_fio_paths[SMOOTHAF_FIO_PATH_COUNT] =
{
    "SMOOTHAF-ROOT-TEST.LOG",
    "B:/SMOOTHAF-ROOT-TEST.LOG",
    "A:/SMOOTHAF-ROOT-TEST.LOG",
    "ML/SMOOTHAF-ROOT-TEST.LOG",
    "B:/AUTOEXEC.BIN",
    "B:/ML/MODULES/smoothaf.mo",
    "B:/SMAFRT.LOG",
};

static const char *smoothaf_fio_phase_name(enum smoothaf_fio_phase phase)
{
    switch (phase)
    {
        case SMOOTHAF_FIO_BLOCKED_TASK: return "BLOCKED TASK";
        case SMOOTHAF_FIO_NAME_CHECK: return "NAME_CHECK";
        case SMOOTHAF_FIO_NOT_FOUND: return "NOT_FOUND";
        case SMOOTHAF_FIO_COLLISION: return "COLLISION";
        case SMOOTHAF_FIO_SIZE_CHECK: return "SIZE_CHECK";
        case SMOOTHAF_FIO_SIZE_MISMATCH: return "SIZE_MISMATCH";
        case SMOOTHAF_FIO_OPEN: return "OPEN";
        case SMOOTHAF_FIO_SEEK_SET: return "SEEK_SET";
        case SMOOTHAF_FIO_WRITE: return "WRITE";
        case SMOOTHAF_FIO_CLOSE: return "CLOSE";
        case SMOOTHAF_FIO_SUCCESS: return "SUCCESS";
        case SMOOTHAF_FIO_ERROR: return "ERROR";
        case SMOOTHAF_FIO_ABI_UNAVAILABLE: return "ABI_UNAVAILABLE";
        case SMOOTHAF_FIO_PATH_SCAN: return "PATH_SCAN";
        default: return "READY";
    }
}

static int smoothaf_fio_reserve_root(struct smoothaf_fio_state *state)
{
    if (smoothaf_fio_shutdown ||
        (state != &smoothaf_fio_create_state && smoothaf_fio_create_state.reserved) ||
        (state != &smoothaf_fio_edit_state && smoothaf_fio_edit_state.reserved) ||
        (state != &smoothaf_fio_paths_state && smoothaf_fio_paths_state.reserved))
        return 0;
    return smoothaf_fio_reserve(state, smoothaf_is_task_running(),
                                smoothaf_task_exited, smoothaf_monitor.enabled);
}

static MENU_UPDATE_FUNC(smoothaf_fio_create_update)
{
    MENU_SET_VALUE("%s handle=%s bytes=%d close=%s ML_CARD/root",
                   smoothaf_fio_phase_name(smoothaf_fio_create_state.phase),
                   smoothaf_fio_create_state.handle_valid ? "VALID" : "NULL",
                   (int)smoothaf_fio_create_state.bytes_written,
                   smoothaf_fio_create_state.close_called ? "CALLED" : "NO");
}

static MENU_UPDATE_FUNC(smoothaf_fio_edit_update)
{
    MENU_SET_VALUE("%s size=%d open=%d seek=%d bytes=%d close=%s ML_CARD/root",
                   smoothaf_fio_phase_name(smoothaf_fio_edit_state.phase),
                   (int)smoothaf_fio_edit_state.file_size,
                   smoothaf_fio_edit_state.open_return,
                   smoothaf_fio_edit_state.seek_return,
                   (int)smoothaf_fio_edit_state.bytes_written,
                   smoothaf_fio_edit_state.close_called ? "CALLED" : "NO");
}

static MENU_UPDATE_FUNC(smoothaf_fio_paths_update)
{
    MENU_SET_VALUE("%s", smoothaf_fio_phase_name(smoothaf_fio_paths_state.phase));
}

static MENU_UPDATE_FUNC(smoothaf_fio_path_entry_update)
{
    MENU_SET_VALUE("%d %s", smoothaf_fio_path_entry + 1,
                   smoothaf_fio_paths[smoothaf_fio_path_entry]);
}

static MENU_SELECT_FUNC(smoothaf_fio_path_entry_select)
{
    smoothaf_fio_path_entry++;
    if (smoothaf_fio_path_entry >= SMOOTHAF_FIO_PATH_COUNT)
        smoothaf_fio_path_entry = 0;
}

static MENU_UPDATE_FUNC(smoothaf_fio_ml_card_update)
{
    struct card_info *card = get_ml_card();
    MENU_SET_VALUE("%s", card && card->drive_letter ? card->drive_letter : "N/A");
}

static MENU_UPDATE_FUNC(smoothaf_fio_shooting_card_update)
{
    struct card_info *card = get_shooting_card();
    MENU_SET_VALUE("%s", card && card->drive_letter ? card->drive_letter : "N/A");
}

static MENU_UPDATE_FUNC(smoothaf_fio_path_result_update)
{
    struct smoothaf_fio_path_state *result =
        &smoothaf_fio_path_results[smoothaf_fio_path_entry];
    MENU_SET_VALUE("%s is_call=%d is=%d size_call=%d size_rc=%d size=%u open_call=%d open=%d h=%08x close=%s",
                   smoothaf_fio_paths[smoothaf_fio_path_entry],
                   result->is_file_called, result->is_file_return,
                   result->size_called, result->size_return,
                   result->file_size, result->open_called,
                   result->open_return, (unsigned int)result->open_handle,
                   result->close_called ? "CALLED" : "NO");
}

static MENU_SELECT_FUNC(smoothaf_fio_paths_select)
{
    int i;
    uint32_t size;
    FILE *file;

    if (!smoothaf_fio_reserve_root(&smoothaf_fio_paths_state))
    {
        smoothaf_fio_blocked(&smoothaf_fio_paths_state);
        return;
    }
    smoothaf_fio_paths_state.phase = SMOOTHAF_FIO_PATH_SCAN;
    for (i = 0; i < SMOOTHAF_FIO_PATH_COUNT; i++)
    {
        size = 0;
        file = 0;
        smoothaf_fio_path_state_init(&smoothaf_fio_path_results[i]);
        smoothaf_fio_path_results[i].is_file_called = 1;
        smoothaf_fio_path_results[i].is_file_return =
            is_file(smoothaf_fio_paths[i]);
        smoothaf_fio_path_results[i].size_called = 1;
        smoothaf_fio_path_results[i].size_return =
            FIO_GetFileSize(smoothaf_fio_paths[i], &size);
        smoothaf_fio_path_results[i].file_size = size;
        smoothaf_fio_path_results[i].open_called = 1;
        file = FIO_OpenFile(smoothaf_fio_paths[i], O_RDONLY);
        smoothaf_fio_path_results[i].open_return = file != 0;
        smoothaf_fio_path_results[i].open_handle = (uintptr_t)file;
        smoothaf_fio_path_results[i].handle_valid = file != 0;
        if (file)
        {
            FIO_CloseFile(file);
            smoothaf_fio_path_results[i].close_called = 1;
        }
        smoothaf_fio_path_result(&smoothaf_fio_path_results[i],
                                 smoothaf_fio_path_results[i].is_file_called,
                                 smoothaf_fio_path_results[i].is_file_return,
                                 smoothaf_fio_path_results[i].size_called,
                                 smoothaf_fio_path_results[i].size_return,
                                 smoothaf_fio_path_results[i].file_size,
                                 smoothaf_fio_path_results[i].open_called,
                                 smoothaf_fio_path_results[i].open_return,
                                 smoothaf_fio_path_results[i].open_handle,
                                 smoothaf_fio_path_results[i].close_called);
    }
    smoothaf_fio_paths_state.published = 1;
    smoothaf_fio_release(&smoothaf_fio_paths_state);
}

static MENU_SELECT_FUNC(smoothaf_fio_root_create_select)
{
    static const char content[] = SMOOTHAF_FIO_ROOT_CREATE_CONTENT;
    FILE *file;
    int write_result;

    if (!smoothaf_fio_reserve_root(&smoothaf_fio_create_state))
    {
        smoothaf_fio_blocked(&smoothaf_fio_create_state);
        return;
    }
    smoothaf_fio_name_check(&smoothaf_fio_create_state);
    if (is_file(SMOOTHAF_FIO_ROOT_CREATE_PATH))
    {
        smoothaf_fio_collision(&smoothaf_fio_create_state);
        smoothaf_fio_release(&smoothaf_fio_create_state);
        return;
    }
    file = FIO_CreateFile(SMOOTHAF_FIO_ROOT_CREATE_PATH);
    if (!file)
    {
        smoothaf_fio_error(&smoothaf_fio_create_state);
        smoothaf_fio_release(&smoothaf_fio_create_state);
        return;
    }
    smoothaf_fio_opened(&smoothaf_fio_create_state, 1);
    write_result = FIO_WriteFile(file, content, sizeof(content) - 1);
    smoothaf_fio_write_result(&smoothaf_fio_create_state, write_result,
                              sizeof(content) - 1);
    FIO_CloseFile(file);
    smoothaf_fio_closed(&smoothaf_fio_create_state);
    smoothaf_fio_publish(&smoothaf_fio_create_state,
                         write_result == (int)(sizeof(content) - 1));
    smoothaf_fio_release(&smoothaf_fio_create_state);
}

static MENU_SELECT_FUNC(smoothaf_fio_root_edit_select)
{
    static const char content[] = SMOOTHAF_FIO_ROOT_EDIT_CONTENT;
    FILE *file;
    uint32_t size = 0;
    int seek_result;
    int write_result;

    if (!smoothaf_fio_reserve_root(&smoothaf_fio_edit_state))
    {
        smoothaf_fio_blocked(&smoothaf_fio_edit_state);
        return;
    }
    smoothaf_fio_name_check(&smoothaf_fio_edit_state);
    if (!is_file(SMOOTHAF_FIO_ROOT_EDIT_PATH))
    {
        smoothaf_fio_not_found(&smoothaf_fio_edit_state);
        smoothaf_fio_release(&smoothaf_fio_edit_state);
        return;
    }
    if (FIO_GetFileSize(SMOOTHAF_FIO_ROOT_EDIT_PATH, &size) != 0)
    {
        smoothaf_fio_error(&smoothaf_fio_edit_state);
        smoothaf_fio_release(&smoothaf_fio_edit_state);
        return;
    }
    smoothaf_fio_size_check(&smoothaf_fio_edit_state, size);
    if (size != SMOOTHAF_FIO_ROOT_EDIT_SIZE)
    {
        smoothaf_fio_size_mismatch(&smoothaf_fio_edit_state);
        smoothaf_fio_release(&smoothaf_fio_edit_state);
        return;
    }
    file = FIO_OpenFile(SMOOTHAF_FIO_ROOT_EDIT_PATH, O_RDWR | O_SYNC);
    smoothaf_fio_opened(&smoothaf_fio_edit_state, file != 0);
    if (!file)
    {
        smoothaf_fio_release(&smoothaf_fio_edit_state);
        return;
    }
    seek_result = FIO_SeekSkipFile(file, 0, SEEK_SET);
    smoothaf_fio_seeked(&smoothaf_fio_edit_state, seek_result);
    if (seek_result != 0)
    {
        FIO_CloseFile(file);
        smoothaf_fio_closed(&smoothaf_fio_edit_state);
        smoothaf_fio_publish(&smoothaf_fio_edit_state, 0);
        smoothaf_fio_release(&smoothaf_fio_edit_state);
        return;
    }
    write_result = FIO_WriteFile(file, content, sizeof(content) - 1);
    smoothaf_fio_write_result(&smoothaf_fio_edit_state, write_result,
                              sizeof(content) - 1);
    FIO_CloseFile(file);
    smoothaf_fio_closed(&smoothaf_fio_edit_state);
    smoothaf_fio_publish(&smoothaf_fio_edit_state,
                         write_result == (int)(sizeof(content) - 1));
    smoothaf_fio_release(&smoothaf_fio_edit_state);
}


#endif

#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
static MENU_UPDATE_FUNC(smoothaf_sd_test_update)
{
    if (smoothaf_sd_test_success)
        MENU_SET_VALUE("SUCCESS CLOSE=CALLED");
    else if (smoothaf_sd_test_collision)
        MENU_SET_VALUE("BLOCKED COLLISION");
    else if (smoothaf_sd_test_stage == SMOOTHAF_SD_TEST_STAGE_FILE_CREATE)
        MENU_SET_VALUE("ERROR FILE_CREATE handle=NULL");
    else if (smoothaf_sd_test_stage == SMOOTHAF_SD_TEST_STAGE_WRITE)
        MENU_SET_VALUE("ERROR WRITE raw=%d", smoothaf_sd_test_raw_return);
    else if (smoothaf_sd_test_stage == SMOOTHAF_SD_TEST_STAGE_TASK_GATE)
        MENU_SET_VALUE("BLOCKED TASK_STATE");
    else if (smoothaf_sd_test_stage)
        MENU_SET_VALUE("ERROR %s raw=%d", smoothaf_sd_test_stage_name(smoothaf_sd_test_stage),
                       smoothaf_sd_test_raw_return);
    else
        MENU_SET_VALUE("READY");
}

static MENU_SELECT_FUNC(smoothaf_sd_test_select)
{
    static const char content[] = SMOOTHAF_SD_TEST_CONTENT;
    FILE *file;
    int write_result;

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_TASK_GATE;
    smoothaf_sd_test_raw_return = 0;
    smoothaf_sd_test_collision = 0;
    smoothaf_sd_test_success = 0;
    if (smoothaf_is_task_running() || !smoothaf_task_exited)
        return;

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_DIRECTORY;
    if (!is_dir("ML/LOGS"))
    {
        smoothaf_sd_test_raw_return = FIO_CreateDirectory("ML/LOGS");
        if (smoothaf_sd_test_raw_return != 0)
            return;
    }

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_NAME_CHECK;
    if (is_file(SMOOTHAF_SD_TEST_PATH))
    {
        smoothaf_sd_test_collision = 1;
        return;
    }

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_FILE_CREATE;
    file = FIO_CreateFile(SMOOTHAF_SD_TEST_PATH);
    if (!file)
        return;

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_WRITE;
    write_result = FIO_WriteFile(file, content, sizeof(content) - 1);
    smoothaf_sd_test_raw_return = write_result;
    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_CLOSE;
    FIO_CloseFile(file);
    if (write_result != (int)(sizeof(content) - 1))
        return;

    smoothaf_sd_test_stage = SMOOTHAF_SD_TEST_STAGE_SUCCESS;
    smoothaf_sd_test_success = 1;
}
#endif

/* Measurement is allowed in MF: it is read-only and useful as a diagnostic.
 * Lens movement and the search use the stricter smoothaf_block_reason(). */
static int smoothaf_measure_block_reason(void)
{
    if (smoothaf_cancel) return SMOOTHAF_BLOCK_CANCELLED;
    if (!lv) return SMOOTHAF_BLOCK_NO_LV;
    if (is_movie_mode()) return SMOOTHAF_BLOCK_MOVIE;
    if (RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW)
        return SMOOTHAF_BLOCK_RECORDING;
    return SMOOTHAF_BLOCK_NONE;
}

static int smoothaf_block_reason(void)
{
    int reason = smoothaf_measure_block_reason();
    if (reason) return reason;

    if (is_manual_focus()) return SMOOTHAF_BLOCK_MANUAL_FOCUS;
    if (is_continuous_af()) return SMOOTHAF_BLOCK_CONTINUOUS_AF;

    /* is_follow_focus_active() reports only an *active* Follow Focus (it is
     * false while the ML menu is open).  If the user closes the menu during a
     * search, this check stops the next command.  Rack Focus and Focus
     * Stacking expose only their configured/running state, so the module is
     * conservative and refuses to move whenever either is enabled. */
    if (is_follow_focus_active()) return SMOOTHAF_BLOCK_FOLLOW_FOCUS;
    if (is_rack_focus_enabled()) return SMOOTHAF_BLOCK_RACK_FOCUS;
    if (is_focus_stack_enabled()) return SMOOTHAF_BLOCK_FOCUS_STACK;

    if (smoothaf_step < 1 || smoothaf_step > 3) return SMOOTHAF_BLOCK_PARAMS;
    return SMOOTHAF_BLOCK_NONE;
}

/* Read the central 64x64 ROI from the current YUV422 LiveView buffer.
 * Returns 1 on success, 0 when the buffer or the camera state is unusable. */
static int smoothaf_measure(int *score_out, int *mean_out,
                            int *sad_out, int *laplacian_out)
{
    struct vram_info *vram;
    const uint8_t *base;
    int left;
    int top;
    int sad;
    int laplacian;
    int mean;

    if (score_out) *score_out = -1;
    if (mean_out) *mean_out = 0;
    if (sad_out) *sad_out = 0;
    if (laplacian_out) *laplacian_out = 0;

    if (!lv) return 0;
    if (is_movie_mode() || RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW)
        return 0;

    vram = get_yuv422_vram();
    if (!vram || !vram->vram) return 0;
    if (!smoothaf_geometry_ok(vram->width, vram->height, vram->pitch))
        return 0;

    left = smoothaf_roi_left(vram->width);
    top = smoothaf_roi_top(vram->height);

    /* Bounds are guaranteed by smoothaf_geometry_ok() and the ROI helpers:
     * left/top are >= 0, left is even, and the last read byte is inside the
     * last ROI row (pitch >= width * 2). */
    base = vram->vram + top * vram->pitch + left * 2;
    smoothaf_measure_all(base, vram->pitch, &sad, &laplacian, &mean);

    if (score_out)
    {
        *score_out = smoothaf_metric == SMOOTHAF_METRIC_LAPLACIAN
                   ? laplacian : sad;
    }
    if (mean_out) *mean_out = mean;
    if (sad_out) *sad_out = sad;
    if (laplacian_out) *laplacian_out = laplacian;
    return 1;
}

/* Issue exactly one synchronous lens command.  Only the SmoothAF task calls
 * this function, and smoothaf_task_running guarantees a single SmoothAF task. */
static int smoothaf_move(int direction)
{
    int start;
    int result;
    int reason;

    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;

    if (direction != -1 && direction != 1)
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_PARAMS;
        smoothaf_last_error = 1;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR, SMOOTHAF_BLOCK_PARAMS,
                                    direction, smoothaf_step, 0, 0);
        return 0;
    }
    if (smoothaf_step < 1 || smoothaf_step > 3)
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_PARAMS;
        smoothaf_last_error = 1;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR, SMOOTHAF_BLOCK_PARAMS,
                                    direction, smoothaf_step, 0, 0);
        return 0;
    }

    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ABORT, reason, direction,
                                    smoothaf_step, 0, 0);
        return 0;
    }

    if (smoothaf_af_move_attempts != INT32_MAX)
        smoothaf_af_move_attempts++;
    smoothaf_status = SMOOTHAF_STATUS_MOVING;
    start = get_ms_clock();

    /* wait=1 is intentional: one command at a time, confirmed by the core. */
    smoothaf_last_lens_called = 1;
    if (smoothaf_af_lens_calls != INT32_MAX)
        smoothaf_af_lens_calls++;
    smoothaf_log_event(SMOOTHAF_LOG_MOVE, direction, smoothaf_step, 0);
    result = lens_focus(direction, smoothaf_step, 1, 0);
    smoothaf_last_lens_return = result;
    smoothaf_log_event_extended(SMOOTHAF_LOG_CONTROLLER, direction, smoothaf_step,
                                smoothaf_last_focus_pos, result,
                                smoothaf_last_block);
    if (result && smoothaf_af_move_successes != INT32_MAX)
        smoothaf_af_move_successes++;

    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - (uint32_t)start);
    smoothaf_last_focus_pos = lens_info.focus_pos;

    if (!result)
    {
        reason = smoothaf_block_reason();
        if (reason) smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR,
                                    smoothaf_last_block, result,
                                    smoothaf_last_lens_return,
                                    smoothaf_af_move_attempts,
                                    smoothaf_af_current_position);
        return 0;
    }

    msleep(SMOOTHAF_COOLDOWN_MS);
    return 1;
}

/* ---------------------------------------------------------------- */
/* Search driver                                                     */
/* ---------------------------------------------------------------- */

static int smoothaf_search_move(void *ctx, int dir)
{
    (void)ctx;
    return smoothaf_move(dir);
}

static int smoothaf_search_measure(void *ctx, int *score)
{
    int mean = 0;
    int sad = 0;
    int laplacian = 0;

    (void)ctx;
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    msleep(SMOOTHAF_SETTLE_MS);

    if (!smoothaf_measure(score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        return 0;
    }

    smoothaf_publish_measurement(*score, mean, sad, laplacian, 1,
                                 smoothaf_has_confidence(*score, mean)
                                 ? SMOOTHAF_MEASURE_OK
                                 : SMOOTHAF_MEASURE_LOW);
    return 1;
}

static int smoothaf_search_abort(void *ctx)
{
    uint32_t start = *(uint32_t *)ctx;
    int reason;

    if (smoothaf_cancel)
    {
        return SMOOTHAF_RESULT_CANCELLED;
    }

    /* The timeout is checked between commands.  lens_focus(wait=1) is
     * blocking and cannot be interrupted, so 5 s is a best-effort limit. */
    if ((uint32_t)get_ms_clock() - start > SMOOTHAF_TIMEOUT_MS)
    {
        return SMOOTHAF_RESULT_TIMEOUT;
    }

    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        if (reason == SMOOTHAF_BLOCK_CANCELLED ||
            reason == SMOOTHAF_BLOCK_DISABLED)
        {
            return SMOOTHAF_RESULT_CANCELLED;
        }
        return SMOOTHAF_RESULT_ERROR;
    }

    return 0;
}

static int smoothaf_status_from_result(enum smoothaf_result result)
{
    switch (result)
    {
        case SMOOTHAF_RESULT_LOCKED:        return SMOOTHAF_STATUS_LOCKED;
        case SMOOTHAF_RESULT_LIMIT:         return SMOOTHAF_STATUS_SEARCH_LIMIT;
        case SMOOTHAF_RESULT_NO_CONFIDENCE: return SMOOTHAF_STATUS_NO_CONFIDENCE;
        case SMOOTHAF_RESULT_CANCELLED:     return SMOOTHAF_STATUS_CANCELLED;
        case SMOOTHAF_RESULT_TIMEOUT:       return SMOOTHAF_STATUS_TIMEOUT;
        case SMOOTHAF_RESULT_LENS_ERROR:    return SMOOTHAF_STATUS_LENS_ERROR;
        default:                          return SMOOTHAF_STATUS_ERROR;
    }
}

/* ---------------------------------------------------------------- */
/* Tasks                                                             */
/* ---------------------------------------------------------------- */

static void smoothaf_monitor_task(void)
{
    uint32_t completed;
    uint32_t start;
    int score = -1;
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    int valid;
    int result_processed;
    struct smoothaf_monitor_sample sample;

    while (smoothaf_monitor.enabled && !smoothaf_cancel &&
           !smoothaf_monitor_deinit && !smoothaf_task_stop_requested)
    {
        if (smoothaf_monitor_due((uint32_t)get_ms_clock(),
                                 smoothaf_monitor.metrics.next_due_ms))
        {
            if (smoothaf_measure_block_reason())
            {
                if (smoothaf_vlog_af.armed && smoothaf_vlog_af.awaiting_result)
                {
                    smoothaf_last_block = smoothaf_measure_block_reason();
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                smoothaf_monitor_record_blocked(&smoothaf_monitor);
                smoothaf_monitor_record_skip(&smoothaf_monitor);
                smoothaf_monitor_schedule_after(&smoothaf_monitor,
                                                (uint32_t)get_ms_clock());
                smoothaf_monitor_transition(&smoothaf_monitor,
                                            SMOOTHAF_MONITOR_PAUSED);
                smoothaf_monitor_log_transitions();
                msleep(20);
                continue;
            }

            start = (uint32_t)get_ms_clock();
            smoothaf_monitor_record_start(&smoothaf_monitor, start);
            if (smoothaf_monitor.metrics.attempts != UINT32_MAX)
                smoothaf_monitor.metrics.attempts++;
            valid = smoothaf_measure(&score, &mean, &sad, &laplacian);
            completed = (uint32_t)get_ms_clock();
            sample.score = score;
            sample.mean = mean;
            sample.sad = sad;
            sample.laplacian = laplacian;
            sample.valid = valid;
            sample.confident = valid && smoothaf_has_confidence(score, mean);
            sample.metric = smoothaf_metric == SMOOTHAF_METRIC_LAPLACIAN
                          ? SMOOTHAF_MONITOR_METRIC_LAPLACIAN
                          : SMOOTHAF_MONITOR_METRIC_SAD;
            sample.duration_ms = completed - start;
            if (sample.duration_ms > smoothaf_monitor_interval_ms(
                    smoothaf_monitor.interval_index) &&
                smoothaf_monitor.metrics.overruns != UINT32_MAX)
                smoothaf_monitor.metrics.overruns++;
            /* A settling/post-move frame belongs only to the AF episode.
             * It must not update the passive monitor reference or blur state. */
            if (!smoothaf_vlog_af.awaiting_result)
                smoothaf_monitor_accept(&smoothaf_monitor, &sample, completed);
            else
                smoothaf_monitor_schedule_after(&smoothaf_monitor, completed);
            result_processed = 0;
            if (smoothaf_vlog_af.armed &&
                smoothaf_vlog_af_result_due(&smoothaf_vlog_af, completed))
            {
                if (smoothaf_vlog_af.started_ms &&
                    (uint32_t)(completed - smoothaf_vlog_af.started_ms) >
                    SMOOTHAF_VLOG_AF_TIMEOUT_MS)
                {
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_TIMEOUT;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                else if (!sample.valid || !sample.confident ||
                         smoothaf_monitor.state == SMOOTHAF_MONITOR_UNCERTAIN)
                {
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
                    smoothaf_vlog_af.uncertain = 1;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                else
                {
                    smoothaf_vlog_af_record_result(&smoothaf_vlog_af,
                                                   sample.score, completed);
                    result_processed = 1;
                    smoothaf_af_post_move_score = smoothaf_vlog_af.post_move_score;
                    smoothaf_af_best_score = smoothaf_vlog_af.best;
                    smoothaf_af_score_delta = smoothaf_vlog_af.score_delta;
                    smoothaf_af_best_position = smoothaf_vlog_af.best_position;
                    smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                    smoothaf_af_last_direction = smoothaf_vlog_af.last_direction;
                    smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
                }
            }
            if (smoothaf_vlog_af.armed &&
                smoothaf_monitor.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR &&
                smoothaf_vlog_af.state == SMOOTHAF_VLOG_AF_WAITING &&
                smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_OFF)
            {
                smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_PROBING;
            }
            if (!result_processed && smoothaf_vlog_af.armed &&
                smoothaf_vlog_af.state == SMOOTHAF_VLOG_AF_PROBING &&
                !smoothaf_vlog_af.awaiting_result)
            {
                int can_command = smoothaf_vlog_af_can_command(&smoothaf_vlog_af,
                                                                 smoothaf_enabled,
                                                                 completed);
                if (!can_command)
                {
                    if (smoothaf_vlog_af.mode == SMOOTHAF_VLOG_AF_ACTIVE &&
                        !smoothaf_enabled)
                        smoothaf_last_block = SMOOTHAF_BLOCK_DISABLED;
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                else if (smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_DRY_RUN)
                {
                    smoothaf_vlog_af_dry_run_request(&smoothaf_vlog_af,
                                                    smoothaf_enabled,
                                                    completed);
                    smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_DRY_RUN;
                }
                else if (smoothaf_block_reason())
                {
                    smoothaf_last_block = smoothaf_block_reason();
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                else if (!smoothaf_move(smoothaf_vlog_af.direction))
                {
                    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_FAILED;
                    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
                }
                else
                {
                    smoothaf_vlog_af_record_command(&smoothaf_vlog_af,
                                                    (uint32_t)get_ms_clock());
                    smoothaf_vlog_af.current_position += smoothaf_vlog_af.direction;
                    smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
                    smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                    smoothaf_af_last_direction = smoothaf_vlog_af.direction;
                }
            }
            smoothaf_monitor_log_transitions();
        }
        else
        {
            msleep(20);
        }
    }
    smoothaf_monitor_task_requested = 0;
}

static void smoothaf_measure_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    int score = -1;
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    int reason;

    smoothaf_status = SMOOTHAF_STATUS_MEASURING;

    smoothaf_last_measure_valid = 0;
    reason = smoothaf_measure_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_publish_measurement(-1, 0, 0, 0, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = (reason == SMOOTHAF_BLOCK_CANCELLED ||
                         reason == SMOOTHAF_BLOCK_DISABLED)
                        ? SMOOTHAF_STATUS_CANCELLED
                        : SMOOTHAF_STATUS_ERROR;
        goto done;
    }

    if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        goto done;
    }

    smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                 smoothaf_has_confidence(score, mean)
                                 ? SMOOTHAF_MEASURE_OK
                                 : SMOOTHAF_MEASURE_LOW);
    smoothaf_status = smoothaf_has_confidence(score, mean)
                    ? SMOOTHAF_STATUS_IDLE
                    : SMOOTHAF_STATUS_NO_CONFIDENCE;

done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
}

static void smoothaf_step_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    int score = -1;
    int mean = 0;
    int reason;

    smoothaf_status = SMOOTHAF_STATUS_MEASURING;

    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_status = (reason == SMOOTHAF_BLOCK_CANCELLED ||
                         reason == SMOOTHAF_BLOCK_DISABLED)
                        ? SMOOTHAF_STATUS_CANCELLED
                        : SMOOTHAF_STATUS_ERROR;
        goto done;
    }

    if (!smoothaf_move(smoothaf_direction_value()))
    {
        smoothaf_last_error = 1;
        if (smoothaf_last_block != SMOOTHAF_BLOCK_NONE)
        {
            smoothaf_status = (smoothaf_last_block == SMOOTHAF_BLOCK_CANCELLED ||
                             smoothaf_last_block == SMOOTHAF_BLOCK_DISABLED)
                            ? SMOOTHAF_STATUS_CANCELLED
                            : SMOOTHAF_STATUS_ERROR;
        }
        else
        {
            smoothaf_status = SMOOTHAF_STATUS_LENS_ERROR;
        }
        goto done;
    }

    /* A Stop may have arrived while lens_focus() was blocking.  Do not issue
     * another command, but a read-only measurement is still harmless. */
    if (smoothaf_cancel)
    {
        smoothaf_status = SMOOTHAF_STATUS_CANCELLED;
        goto done;
    }

    msleep(SMOOTHAF_SETTLE_MS);

    {
        int sad = 0;
        int laplacian = 0;

        if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
        {
            smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                         SMOOTHAF_MEASURE_ERROR);
            smoothaf_status = SMOOTHAF_STATUS_ERROR;
            goto done;
        }

        smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                     smoothaf_has_confidence(score, mean)
                                     ? SMOOTHAF_MEASURE_OK
                                     : SMOOTHAF_MEASURE_LOW);
    }
    smoothaf_status = smoothaf_has_confidence(score, mean)
                    ? SMOOTHAF_STATUS_IDLE
                    : SMOOTHAF_STATUS_NO_CONFIDENCE;

done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
}

static enum smoothaf_result smoothaf_normal_run(
        struct smoothaf_normal_af *normal, int initial_score,
        uint32_t *start)
{
    enum smoothaf_normal_action action;
    int direction;
    int step;
    int score;
    int stop;
    int physical_step;

    smoothaf_normal_af_init(normal, initial_score,
                            smoothaf_direction_value(), smoothaf_step);
    action = smoothaf_normal_af_begin(normal);
    for (;;)
    {
        if (action == SMOOTHAF_NORMAL_LOCKED)
            return SMOOTHAF_RESULT_LOCKED;
        if (action == SMOOTHAF_NORMAL_LIMIT)
            return SMOOTHAF_RESULT_LIMIT;
        if (action == SMOOTHAF_NORMAL_NO_CONFIDENCE)
            return SMOOTHAF_RESULT_NO_CONFIDENCE;

        stop = smoothaf_search_abort(start);
        if (stop)
            return (enum smoothaf_result)stop;

        if (action == SMOOTHAF_NORMAL_STEP)
        {
            if (!smoothaf_normal_af_next_step(normal, &direction, &step))
                return SMOOTHAF_RESULT_LIMIT;
            physical_step = smoothaf_step;
            if (step != physical_step)
                return SMOOTHAF_RESULT_ERROR;
            if (!smoothaf_move(direction))
                return SMOOTHAF_RESULT_LENS_ERROR;
            stop = smoothaf_search_abort(start);
            if (stop)
                return (enum smoothaf_result)stop;
        }

        if (!smoothaf_search_measure(0, &score))
            return SMOOTHAF_RESULT_MEASURE_ERROR;
        stop = smoothaf_search_abort(start);
        if (stop)
            return (enum smoothaf_result)stop;
        action = smoothaf_normal_af_on_score(normal, score);
    }
}

static enum smoothaf_result smoothaf_adaptive_run(
        struct smoothaf_adaptive_hc *adaptive, int initial_score,
        uint32_t *start)
{
    enum smoothaf_adaptive_action action;
    struct smoothaf_adaptive_policy policy;
    int direction;
    int abstract_step;
    int score;
    int stop;

    /* Keep controller coordinates identical to physical lens movement.  The
     * Legacy lens API has no independently validated num_steps dimension, so
     * Adaptive HC must not schedule an abstract displacement different from
     * the configured physical Step Size. */
    smoothaf_adaptive_policy_default(&policy);
    policy.min_step = smoothaf_step;
    policy.initial_step = smoothaf_step;
    policy.max_step = smoothaf_step;
    smoothaf_adaptive_hc_init(adaptive, &policy, initial_score,
                              smoothaf_direction_value());
    action = smoothaf_adaptive_hc_begin(adaptive);

    for (;;)
    {
        if (action == SMOOTHAF_ADAPTIVE_LOCKED)
        {
            return SMOOTHAF_RESULT_LOCKED;
        }
        if (action == SMOOTHAF_ADAPTIVE_NO_PROGRESS)
        {
            return SMOOTHAF_RESULT_LIMIT;
        }
        if (action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE)
        {
            return SMOOTHAF_RESULT_NO_CONFIDENCE;
        }
        if (action == SMOOTHAF_ADAPTIVE_LIMIT)
        {
            return SMOOTHAF_RESULT_LIMIT;
        }

        stop = smoothaf_search_abort(start);
        if (stop)
        {
            smoothaf_log_event(SMOOTHAF_LOG_ABORT, stop, adaptive->commands,
                               adaptive->reversals);
            return (enum smoothaf_result)stop;
        }

        if (action == SMOOTHAF_ADAPTIVE_STEP)
        {
            if (!smoothaf_adaptive_hc_next_step(adaptive, &direction,
                                                &abstract_step))
            {
                return SMOOTHAF_RESULT_LIMIT;
            }

            /* Adaptive HC changes its abstract search policy only.  Physical
             * movement remains the configured Legacy Step Size. */
            (void)abstract_step;
            smoothaf_log_event_extended(SMOOTHAF_LOG_CONTROLLER, direction,
                                        adaptive->best, adaptive->best_pos,
                                        adaptive->pos, adaptive->direction);
            if (!smoothaf_move(direction))
            {
                return SMOOTHAF_RESULT_LENS_ERROR;
            }

            stop = smoothaf_search_abort(start);
            if (stop)
            {
                smoothaf_log_event(SMOOTHAF_LOG_ABORT, stop,
                                   adaptive->commands, adaptive->reversals);
                return (enum smoothaf_result)stop;
            }
        }

        if (!smoothaf_search_measure(0, &score))
        {
            return SMOOTHAF_RESULT_MEASURE_ERROR;
        }
        stop = smoothaf_search_abort(start);
        if (stop)
        {
            smoothaf_log_event(SMOOTHAF_LOG_ABORT, stop, adaptive->commands,
                               adaptive->reversals);
            return (enum smoothaf_result)stop;
        }

        action = smoothaf_adaptive_hc_on_score(adaptive, score);
        smoothaf_log_event(SMOOTHAF_LOG_CONTROLLER, action, adaptive->best,
                           adaptive->best_pos);
    }
}

static void smoothaf_search_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    struct smoothaf_adaptive_hc adaptive;
    struct smoothaf_normal_af normal;
    int use_adaptive;
    int use_normal;
    struct smoothaf_hc hc;
    int score = -1;
    int mean = 0;
    int reason;
    enum smoothaf_result result = SMOOTHAF_RESULT_ERROR;

    use_adaptive = smoothaf_af_mode == SMOOTHAF_HC_ADAPTIVE;
    use_normal = smoothaf_af_mode == SMOOTHAF_AF_NORMAL;
    smoothaf_hc_init(&hc, 0, 1);
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    smoothaf_last_commands = 0;
    smoothaf_last_reversals = 0;

    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_status = (reason == SMOOTHAF_BLOCK_CANCELLED ||
                         reason == SMOOTHAF_BLOCK_DISABLED)
                        ? SMOOTHAF_STATUS_CANCELLED
                        : SMOOTHAF_STATUS_ERROR;
        goto done;
    }

    {
        int sad = 0;
        int laplacian = 0;

        if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
        {
            smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                         SMOOTHAF_MEASURE_ERROR);
            smoothaf_status = SMOOTHAF_STATUS_ERROR;
            goto done;
        }

        smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                     smoothaf_has_confidence(score, mean)
                                     ? SMOOTHAF_MEASURE_OK
                                     : SMOOTHAF_MEASURE_LOW);
    }

    if (!smoothaf_has_confidence(score, mean))
    {
        smoothaf_status = SMOOTHAF_STATUS_NO_CONFIDENCE;
        goto done;
    }

    if (use_normal)
    {
        result = smoothaf_normal_run(&normal, score, &start);
        smoothaf_last_commands = normal.commands;
        smoothaf_last_reversals = normal.reversals;
        smoothaf_last_best = normal.best;
        smoothaf_last_score = normal.current;
    }
    else if (use_adaptive)
    {
        result = smoothaf_adaptive_run(&adaptive, score, &start);
        smoothaf_last_commands = adaptive.commands;
        smoothaf_last_reversals = adaptive.reversals;
        smoothaf_last_best = adaptive.best;
        smoothaf_last_score = adaptive.current;
    }
    else
    {
        result = smoothaf_hc_run(&hc, score, smoothaf_direction_value(),
                               smoothaf_search_move, smoothaf_search_measure,
                               smoothaf_search_abort, &start);

        smoothaf_last_commands = hc.commands;
        smoothaf_last_reversals = hc.reversals;
        smoothaf_last_best = hc.best;
        smoothaf_last_score = hc.current;
    }

    if (result == SMOOTHAF_RESULT_LENS_ERROR &&
        smoothaf_last_block != SMOOTHAF_BLOCK_NONE)
    {
        smoothaf_status = (smoothaf_last_block == SMOOTHAF_BLOCK_CANCELLED ||
                         smoothaf_last_block == SMOOTHAF_BLOCK_DISABLED)
                        ? SMOOTHAF_STATUS_CANCELLED
                        : SMOOTHAF_STATUS_ERROR;
    }
    else
    {
        smoothaf_status = smoothaf_status_from_result(result);
    }

    if (result == SMOOTHAF_RESULT_LENS_ERROR ||
        result == SMOOTHAF_RESULT_MEASURE_ERROR ||
        result == SMOOTHAF_RESULT_TIMEOUT ||
        result == SMOOTHAF_RESULT_ERROR)
    {
        smoothaf_last_error = 1;
    }

done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
    smoothaf_log_event_extended(SMOOTHAF_LOG_TASK_END, result,
                                smoothaf_last_best, smoothaf_last_focus_pos,
                                smoothaf_last_commands,
                                smoothaf_last_reversals);
}

static void smoothaf_task_entry(void *arg)
{
    int task_id = (int)(intptr_t)arg;

    switch (task_id)
    {
        case SMOOTHAF_TASK_MEASURE: smoothaf_measure_task(); break;
        case SMOOTHAF_TASK_STEP:    smoothaf_step_task();    break;
        case SMOOTHAF_TASK_SEARCH:  smoothaf_search_task();  break;
        case SMOOTHAF_TASK_MONITOR: smoothaf_monitor_task(); break;
        default: break;
    }

    /* Single release point: every task path clears the reservation here.
     * Use the same lock as menu/deinit state transitions. */
    smoothaf_session_finished_ms = (uint32_t)get_ms_clock();
    if (smoothaf_lock())
    {
        smoothaf_task_running = 0;
        smoothaf_task_stop_requested = 0;
        smoothaf_task_exited = 1;
        smoothaf_unlock();
    }
    else
    {
        /* Shutdown may already have made the semaphore unavailable. */
        smoothaf_task_running = 0;
        smoothaf_task_stop_requested = 0;
        smoothaf_task_exited = 1;
    }
}

static int smoothaf_is_task_running(void)
{
    int running;

    if (smoothaf_lock())
    {
        running = smoothaf_task_running;
        smoothaf_unlock();
        return running;
    }

    /* Volatile fallback is only for shutdown/error recovery; normal state
     * transitions use smoothaf_sem. */
    return smoothaf_task_running;
}

/* Reserve the running state before creating the task, so a second menu action
 * cannot create a second task before the first one starts. */
static int smoothaf_start_task(int task_id, int requires_enable)
{
    struct task *task;

    smoothaf_last_lens_called = 0;
    smoothaf_last_lens_return = 0;

    if (!smoothaf_lock())
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_SEMAPHORE;
        smoothaf_last_error = 1;
        return 0;
    }

    if ((requires_enable && !smoothaf_enabled) || smoothaf_task_running)
    {
        smoothaf_unlock();
        return 0;
    }

    smoothaf_task_running = 1;
    smoothaf_task_exited = 0;
    smoothaf_session_started_ms = (uint32_t)get_ms_clock();
    smoothaf_session_finished_ms = 0;
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
    smoothaf_last_export_path[0] = 0;
    smoothaf_last_export_error = 0;
#endif
    /* A new explicit start clears the previous cooperative Stop request. */
    smoothaf_cancel = 0;
    smoothaf_last_error = 0;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    smoothaf_last_lens_called = 0;
    smoothaf_last_lens_return = 0;
    smoothaf_unlock();

    task = task_create("smoothaf_task", SMOOTHAF_TASK_PRIORITY, SMOOTHAF_TASK_STACK,
                       smoothaf_task_entry, (void *)(intptr_t)task_id);

    /* DryOS returns an odd value on failure (see modules/lua/lua_task.c). */
    if (!task || (((uintptr_t)task) & 1))
    {
        if (smoothaf_lock())
        {
            smoothaf_task_running = 0;
            smoothaf_task_exited = 1;
            smoothaf_session_finished_ms = (uint32_t)get_ms_clock();
            smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
            smoothaf_last_error = 1;
            smoothaf_status = SMOOTHAF_STATUS_ERROR;
            smoothaf_unlock();
        }
        else
        {
            /* The reservation must not survive a task-creation failure. */
            smoothaf_task_running = 0;
            smoothaf_task_exited = 1;
            smoothaf_session_finished_ms = (uint32_t)get_ms_clock();
            smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
            smoothaf_last_error = 1;
            smoothaf_status = SMOOTHAF_STATUS_ERROR;
        }
        return 0;
    }

    return 1;
}

/* ---------------------------------------------------------------- */
/* Menu                                                              */
/* ---------------------------------------------------------------- */

static MENU_SELECT_FUNC(smoothaf_measure_select)
{
    smoothaf_start_task(SMOOTHAF_TASK_MEASURE, 0);
}

static MENU_SELECT_FUNC(smoothaf_step_select)
{
    smoothaf_start_task(SMOOTHAF_TASK_STEP, 1);
}

static MENU_SELECT_FUNC(smoothaf_search_select)
{
    smoothaf_start_task(SMOOTHAF_TASK_SEARCH, 1);
}

static MENU_SELECT_FUNC(smoothaf_monitor_toggle_select)
{
    if (smoothaf_monitor.enabled)
    {
        smoothaf_vlog_af_stop(&smoothaf_vlog_af);
        smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
        if (smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_NONE ||
            smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_ARMED)
            smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
        smoothaf_task_stop_requested = 1;
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_task_requested = 0;
        smoothaf_monitor_deinit = 0;
        smoothaf_monitor_log_transitions();
        return;
    }

    if (smoothaf_is_task_running() || !smoothaf_task_exited)
    {
        smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_ERROR);
        smoothaf_monitor_log_transitions();
        return;
    }
    smoothaf_monitor_start(&smoothaf_monitor, smoothaf_metric,
                           (uint32_t)get_ms_clock());
    smoothaf_vlog_af_init(&smoothaf_vlog_af);
    smoothaf_vlog_af_mode = SMOOTHAF_VLOG_AF_OFF;
    smoothaf_monitor_task_requested = 1;
    smoothaf_monitor_deinit = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 0;
    smoothaf_cancel = 0;
    smoothaf_monitor_log_transitions();
    if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))
    {
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_task_requested = 0;
        smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_ERROR);
        smoothaf_monitor_log_transitions();
    }
}

static const char *smoothaf_monitor_task_state_name(void)
{
    if (smoothaf_task_running)
        return smoothaf_task_stop_requested ? "STOPPING" : "RUNNING";
    return smoothaf_task_exited ? "EXITED" : "STOPPED";
}

static void smoothaf_monitor_take_snapshot(
    struct smoothaf_monitor_snapshot *snapshot)
{
    smoothaf_monitor_snapshot(&smoothaf_monitor, snapshot);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_task_state_update)
{
    MENU_SET_VALUE("%s", smoothaf_monitor_task_state_name());
}

static MENU_UPDATE_FUNC(smoothaf_monitor_configured_interval_update)
{
    struct smoothaf_monitor_snapshot snapshot;
    smoothaf_monitor_take_snapshot(&snapshot);
    MENU_SET_VALUE("%d ms", (int)snapshot.interval_ms);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_actual_interval_update)
{
    struct smoothaf_monitor_snapshot snapshot;
    smoothaf_monitor_take_snapshot(&snapshot);
    if (!snapshot.actual_interval_ms)
        MENU_SET_VALUE("N/A");
    else
        MENU_SET_VALUE("%d ms", (int)snapshot.actual_interval_ms);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_last_duration_update)
{
    struct smoothaf_monitor_snapshot snapshot;
    smoothaf_monitor_take_snapshot(&snapshot);
    MENU_SET_VALUE("%d ms", (int)snapshot.duration_last);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_average_duration_update)
{
    struct smoothaf_monitor_snapshot snapshot;
    smoothaf_monitor_take_snapshot(&snapshot);
    if (!snapshot.duration_average)
        MENU_SET_VALUE("N/A");
    else
        MENU_SET_VALUE("%d ms", (int)snapshot.duration_average);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_max_duration_update)
{
    struct smoothaf_monitor_snapshot snapshot;
    smoothaf_monitor_take_snapshot(&snapshot);
    if (!snapshot.duration_max)
        MENU_SET_VALUE("N/A");
    else
        MENU_SET_VALUE("%d ms", (int)snapshot.duration_max);
}

#define SMOOTHAF_MONITOR_COUNTER_UPDATE(name, field) \
static MENU_UPDATE_FUNC(name) \
{ \
    struct smoothaf_monitor_snapshot snapshot; \
    smoothaf_monitor_take_snapshot(&snapshot); \
    MENU_SET_VALUE("%d", (int)snapshot.field); \
}

SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_attempts_update, attempts)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_valid_update, valid)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_invalid_update, invalid)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_low_confidence_update, low_confidence)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_blocked_update, blocked)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_skipped_update, skipped)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_overruns_update, overruns)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_error_update, last_error)

#undef SMOOTHAF_MONITOR_COUNTER_UPDATE

static MENU_UPDATE_FUNC(smoothaf_vlog_af_mode_update)
{
    const char *name = smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE
                     ? "ACTIVE"
                     : smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_DRY_RUN
                     ? "DRY RUN" : "OFF";
    MENU_SET_VALUE("%s", name);
}

static const char *smoothaf_vlog_af_state_name(void)
{
    switch (smoothaf_vlog_af.state)
    {
        case SMOOTHAF_VLOG_AF_PROBING:  return "PROBING";
        case SMOOTHAF_VLOG_AF_SETTLING: return "SETTLING";
        case SMOOTHAF_VLOG_AF_COOLDOWN: return "COOLDOWN";
        case SMOOTHAF_VLOG_AF_COMPLETE: return "COMPLETE";
        case SMOOTHAF_VLOG_AF_ERROR:    return "ERROR";
        default:                        return "WAITING";
    }
}

static MENU_UPDATE_FUNC(smoothaf_vlog_af_state_update)
{
    MENU_SET_VALUE("%s", smoothaf_vlog_af_state_name());
}

static MENU_UPDATE_FUNC(smoothaf_af_move_attempts_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_af_move_attempts);
}

static MENU_UPDATE_FUNC(smoothaf_af_lens_calls_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_af_lens_calls);
}

static MENU_UPDATE_FUNC(smoothaf_af_move_successes_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_af_move_successes);
}

static MENU_UPDATE_FUNC(smoothaf_af_last_block_update)
{
    MENU_SET_VALUE("%s", smoothaf_block_name(smoothaf_last_block));
}

static MENU_UPDATE_FUNC(smoothaf_af_last_move_result_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_last_lens_return);
}

static MENU_UPDATE_FUNC(smoothaf_af_commands_episode_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_af_commands_episode);
}

static const char *smoothaf_vlog_af_result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_VLOG_AF_RESULT_API_SUCCESS: return "API_SUCCESS";
        case SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT: return "NO_MOVEMENT";
        case SMOOTHAF_VLOG_AF_RESULT_BLOCKED: return "BLOCKED";
        case SMOOTHAF_VLOG_AF_RESULT_FAILED: return "API_FAILED";
        case SMOOTHAF_VLOG_AF_RESULT_CANCELLED: return "CANCELLED";
        case SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT: return "NO_IMPROVEMENT";
        case SMOOTHAF_VLOG_AF_RESULT_DRY_RUN: return "DRY_RUN";
        case SMOOTHAF_VLOG_AF_RESULT_ARMED: return "ARMED";
        case SMOOTHAF_VLOG_AF_RESULT_IMPROVED: return "IMPROVED";
        case SMOOTHAF_VLOG_AF_RESULT_WORSE: return "WORSE";
        case SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN: return "UNCERTAIN";
        case SMOOTHAF_VLOG_AF_RESULT_TIMEOUT: return "TIMEOUT";
        case SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT: return "SEARCH_LIMIT";
        case SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED: return "BEST_RECOVERED";
        case SMOOTHAF_VLOG_AF_RESULT_BEST_NOT_RECOVERED: return "BEST_NOT_RECOVERED";
        default: return "NONE";
    }
}

static MENU_UPDATE_FUNC(smoothaf_af_initial_score_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_initial_score); }
static MENU_UPDATE_FUNC(smoothaf_af_post_move_score_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_post_move_score); }
static MENU_UPDATE_FUNC(smoothaf_af_best_score_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_best_score); }
static MENU_UPDATE_FUNC(smoothaf_af_score_delta_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_score_delta); }
static MENU_UPDATE_FUNC(smoothaf_af_best_position_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_best_position); }
static MENU_UPDATE_FUNC(smoothaf_af_current_position_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_current_position); }
static MENU_UPDATE_FUNC(smoothaf_af_last_direction_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_last_direction); }
static MENU_UPDATE_FUNC(smoothaf_af_episode_result_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_result_name(smoothaf_af_episode_result)); }

static MENU_SELECT_FUNC(smoothaf_vlog_af_episode_select)
{
    if (smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_OFF ||
        !smoothaf_monitor.enabled ||
        (smoothaf_is_task_running() && !smoothaf_monitor_task_requested))
        return;
    smoothaf_vlog_af_new_episode(&smoothaf_vlog_af, smoothaf_vlog_af_mode,
                                 smoothaf_direction_value(),
                                 smoothaf_monitor.score);
    smoothaf_af_commands_episode = 0;
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_ARMED;
    smoothaf_af_initial_score = smoothaf_monitor.score;
    smoothaf_af_post_move_score = 0;
    smoothaf_af_best_score = smoothaf_monitor.score;
    smoothaf_af_score_delta = 0;
    smoothaf_af_best_position = 0;
    smoothaf_af_current_position = 0;
    smoothaf_af_last_direction = smoothaf_direction_value();
}

static MENU_UPDATE_FUNC(smoothaf_monitor_status_update)
{
    MENU_SET_VALUE("%s", smoothaf_monitor_state_name());
    MENU_SET_HELP("SAD=%d Lap=%d smooth=%d ref=%d samples=%d blur=%s interval=%ums",
                  (int)smoothaf_monitor.sad,
                  (int)smoothaf_monitor.laplacian,
                  (int)smoothaf_monitor.smoothed,
                  (int)smoothaf_monitor.reference,
                  (int)smoothaf_monitor.metrics.attempts,
                  smoothaf_monitor.possible_blur ? "yes" : "no",
                  (unsigned)smoothaf_monitor_interval_ms(smoothaf_monitor.interval_index));
}

static MENU_UPDATE_FUNC(smoothaf_monitor_sad_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_monitor.sad);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_laplacian_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_monitor.laplacian);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_samples_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_monitor.metrics.attempts);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_smoothed_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_monitor.smoothed);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_reference_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_monitor.reference);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_blur_update)
{
    MENU_SET_VALUE("%s", smoothaf_monitor.possible_blur ? "YES" : "NO");
}

static MENU_SELECT_FUNC(smoothaf_af_assist_trigger_select)
{
    smoothaf_af_assist_request(&smoothaf_af_assist,
                               SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED);
}

static MENU_UPDATE_FUNC(smoothaf_af_assist_update)
{
    MENU_SET_VALUE("%s", smoothaf_af_assist_state_name(
                   smoothaf_af_assist.state));
    MENU_SET_HELP("%s", smoothaf_af_assist_block_name(
                  smoothaf_af_assist.block));
}

static MENU_SELECT_FUNC(smoothaf_stop_select)
{
    if (smoothaf_lock())
    {
        smoothaf_cancel = 1;
        if (!smoothaf_task_running)
        {
            smoothaf_status = SMOOTHAF_STATUS_IDLE;
        }
        smoothaf_unlock();
    }
}

static MENU_UPDATE_FUNC(smoothaf_status_update)
{
    MENU_SET_VALUE("%s", smoothaf_status_name());

    if (smoothaf_last_block)
    {
        MENU_SET_HELP("score=%d mean=%d t=%dms pos=%d blk=%s call=%d ret=%d",
                      smoothaf_last_score, smoothaf_last_mean, smoothaf_last_time,
                      smoothaf_last_focus_pos,
                      smoothaf_block_name(smoothaf_last_block),
                      smoothaf_last_lens_called, smoothaf_last_lens_return);
    }
    else
    {
        MENU_SET_HELP("score=%d best=%d mean=%d t=%dms pos=%d cmds=%d rev=%d",
                      smoothaf_last_score, smoothaf_last_best, smoothaf_last_mean,
                      smoothaf_last_time, smoothaf_last_focus_pos,
                      smoothaf_last_commands, smoothaf_last_reversals);
    }
}

static void smoothaf_diag_refresh(void)
{
    if (!smoothaf_lock())
    {
        return;
    }

    smoothaf_diag_count = smoothaf_log_snapshot(&smoothaf_log,
                                                smoothaf_diag_snapshot,
                                                SMOOTHAF_DIAG_CAPACITY);
    smoothaf_unlock();
    if (smoothaf_diag_count > smoothaf_diag_seen_count)
    {
        smoothaf_diag_page = smoothaf_diag_count - 1;
        smoothaf_diag_seen_count = smoothaf_diag_count;
    }
    if (smoothaf_diag_count == 0)
    {
        smoothaf_diag_page = 0;
    }
    else if (smoothaf_diag_page >= smoothaf_diag_count)
    {
        smoothaf_diag_page = smoothaf_diag_count - 1;
    }
}

static MENU_UPDATE_FUNC(smoothaf_diag_page_update)
{
    smoothaf_diag_refresh();
    MENU_SET_VALUE("%d/%d", smoothaf_diag_count ? (int)smoothaf_diag_page + 1 : 0,
                   (int)smoothaf_diag_count);
    MENU_SET_HELP("Logging %s; Previous/Next select a fixed snapshot.",
                  smoothaf_debug_logging ? "ON" : "OFF");
}

static MENU_UPDATE_FUNC(smoothaf_diag_entry_update)
{
    struct smoothaf_log_entry *record;

    smoothaf_diag_refresh();
    if (smoothaf_diag_count == 0)
    {
        MENU_SET_VALUE("empty");
        MENU_SET_HELP("No records. Enable Debug Logging before a task.");
        return;
    }

    record = &smoothaf_diag_snapshot[smoothaf_diag_page];
    MENU_SET_VALUE("%s t=%ums", smoothaf_log_type_name(record->type),
                   (unsigned)record->timestamp_ms);
    switch (record->type)
    {
        case SMOOTHAF_LOG_MOVE:
            MENU_SET_HELP("dir=%d step=%d pos=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3);
            break;
        case SMOOTHAF_LOG_MEASURE:
            MENU_SET_HELP("score=%d mean=%d pos=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3);
            break;
        case SMOOTHAF_LOG_CONTROLLER:
            MENU_SET_HELP("action=%d best=%d bpos=%d pos=%d dir=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        case SMOOTHAF_LOG_TASK_END:
            MENU_SET_HELP("result=%d best=%d pos=%d cmds=%d rev=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        default:
            MENU_SET_HELP("v1=%d v2=%d v3=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3);
            break;
    }
}

static MENU_SELECT_FUNC(smoothaf_diag_previous_select)
{
    smoothaf_diag_refresh();
    if (smoothaf_diag_page > 0)
    {
        smoothaf_diag_page--;
    }
}

static MENU_SELECT_FUNC(smoothaf_diag_next_select)
{
    smoothaf_diag_refresh();
    if (smoothaf_diag_count && smoothaf_diag_page + 1 < smoothaf_diag_count)
    {
        smoothaf_diag_page++;
    }
}

static MENU_SELECT_FUNC(smoothaf_diag_clear_select)
{
    if (!smoothaf_lock())
    {
        return;
    }

    if (!smoothaf_task_running)
    {
        smoothaf_log_clear(&smoothaf_log);
        smoothaf_diag_count = 0;
        smoothaf_diag_page = 0;
        smoothaf_diag_seen_count = 0;
    }
    smoothaf_unlock();
}

static struct menu_entry smoothaf_menu[] =
{
    {
        .name = "Enable",
        .priv = &smoothaf_enabled,
        .max = 1,
        .help = "Enable lens movement only; Measure and passive Vlog Monitor do not depend on it.",
    },
    {
        .name = "Vlog Monitor",
        .select = menu_open_submenu,
        .children = (struct menu_entry[])
        {
            {
                .name = "Start/Stop Monitoring",
                .select = smoothaf_monitor_toggle_select,
                .help = "Toggle periodic read-only monitoring; no automatic lens movement.",
            },
            {
                .name = "Monitor Status",
                .update = smoothaf_monitor_status_update,
                .help = "Periodic read-only sharpness monitoring; no automatic lens movement.",
            },
            {
                .name = "Vlog AF Mode",
                .priv = &smoothaf_vlog_af_mode,
                .update = smoothaf_vlog_af_mode_update,
                .min = SMOOTHAF_VLOG_AF_OFF,
                .max = SMOOTHAF_VLOG_AF_ACTIVE,
                .choices = CHOICES("OFF", "DRY RUN", "ACTIVE"),
                .help = "Experimental contrast correction; DRY RUN never moves the lens.",
            },
            {
                .name = "Vlog AF State",
                .update = smoothaf_vlog_af_state_update,
                .help = "Episode state; ACTIVE requires Enable and manual arming.",
            },
            {
                .name = "Start New AF Episode",
                .select = smoothaf_vlog_af_episode_select,
                .help = "Manual arm only; no movement is issued by this action.",
            },
            {
                .name = "AF Move Attempts",
                .update = smoothaf_af_move_attempts_update,
            },
            {
                .name = "AF Lens Calls",
                .update = smoothaf_af_lens_calls_update,
            },
            {
                .name = "AF Move Successes",
                .update = smoothaf_af_move_successes_update,
            },
            {
                .name = "AF Last Block",
                .update = smoothaf_af_last_block_update,
            },
            {
                .name = "AF Last Move Result",
                .update = smoothaf_af_last_move_result_update,
            },
            {
                .name = "AF Commands This Episode",
                .update = smoothaf_af_commands_episode_update,
            },
            {
                .name = "AF Episode Result",
                .update = smoothaf_af_episode_result_update,
            },
            {
                .name = "AF Initial Score",
                .update = smoothaf_af_initial_score_update,
            },
            {
                .name = "AF Post-Move Score",
                .update = smoothaf_af_post_move_score_update,
            },
            {
                .name = "AF Best Score",
                .update = smoothaf_af_best_score_update,
            },
            {
                .name = "AF Score Delta",
                .update = smoothaf_af_score_delta_update,
            },
            {
                .name = "AF Best Relative Position",
                .update = smoothaf_af_best_position_update,
            },
            {
                .name = "AF Current Relative Position",
                .update = smoothaf_af_current_position_update,
            },
            {
                .name = "AF Last Direction",
                .update = smoothaf_af_last_direction_update,
            },
            {
                .name = "Monitor Performance",
                .select = menu_open_submenu,
                .children = (struct menu_entry[])
                {
                    { .name = "Task State", .update = smoothaf_monitor_task_state_update },
                    { .name = "Configured Interval", .update = smoothaf_monitor_configured_interval_update },
                    { .name = "Actual Interval", .update = smoothaf_monitor_actual_interval_update },
                    { .name = "Last Duration", .update = smoothaf_monitor_last_duration_update },
                    { .name = "Average Duration", .update = smoothaf_monitor_average_duration_update },
                    { .name = "Maximum Duration", .update = smoothaf_monitor_max_duration_update },
                    { .name = "Attempts", .update = smoothaf_monitor_attempts_update },
                    { .name = "Valid Samples", .update = smoothaf_monitor_valid_update },
                    { .name = "Invalid Samples", .update = smoothaf_monitor_invalid_update },
                    { .name = "Low Confidence", .update = smoothaf_monitor_low_confidence_update },
                    { .name = "Blocked", .update = smoothaf_monitor_blocked_update },
                    { .name = "Skipped", .update = smoothaf_monitor_skipped_update },
                    { .name = "Overruns", .update = smoothaf_monitor_overruns_update },
                    { .name = "Last Error", .update = smoothaf_monitor_error_update },
                    MENU_EOL,
                },
            },
            {
                .name = "SAD Score",
                .update = smoothaf_monitor_sad_update,
                .help = "Latest read-only SAD score.",
            },
            {
                .name = "Laplacian Score",
                .update = smoothaf_monitor_laplacian_update,
                .help = "Latest read-only Laplacian score.",
            },
            {
                .name = "Smoothed Score",
                .update = smoothaf_monitor_smoothed_update,
                .help = "Conservative smoothing of available manual samples.",
            },
            {
                .name = "Reference Score",
                .update = smoothaf_monitor_reference_update,
                .help = "Reference from the first available sample; not a focus guarantee.",
            },
            {
                .name = "Possible Blur",
                .update = smoothaf_monitor_blur_update,
                .help = "Heuristic only; scene changes and lighting can invalidate it.",
            },
            {
                .name = "Sample Count",
                .update = smoothaf_monitor_samples_update,
                .help = "Automatic monitor sampling attempts in the current session.",
            },
            {
                .name = "Sampling Interval",
                .priv = &smoothaf_monitor_interval,
                .min = SMOOTHAF_MONITOR_INTERVAL_1000,
                .max = SMOOTHAF_MONITOR_INTERVAL_100,
                .choices = CHOICES("1000 ms", "500 ms", "250 ms", "100 ms"),
                .help = "Periodic read-only sampling; 100 ms is experimental.",
            },
            MENU_EOL,
        },
    },
    {
        .name = "Diagnostic",
        .select = menu_open_submenu,
        .children = (struct menu_entry[])
        {
            {
                .name = "Measure",
                .select = smoothaf_measure_select,
                .help = "Read-only measurement of the central 64x64 ROI; Enable may remain off.",
            },
            {
                .name = "Focus Step",
                .select = smoothaf_step_select,
                .help = "Execute one explicitly requested focus step.",
            },
            {
                .name = "Direction",
                .priv = &smoothaf_direction_choice,
                .min = 0,
                .max = 1,
                .choices = CHOICES("Direction A", "Direction B"),
                .help = "Relative motor direction; not near/far.",
            },
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
            {
                .name = "Export AF Log",
                .select = smoothaf_export_log_select,
                .update = smoothaf_export_log_update,
                .help = "Export only after Stop and Task State EXITED; never overwrites a log.",
            },
            {
                .name = "Test SD Write",
                .select = smoothaf_sd_test_select,
                .update = smoothaf_sd_test_update,
                .help = "Temporary fixed-content SD test; only after Task State EXITED; never overwrites.",
            },
            {
                .name = "Test Root Create",
                .select = smoothaf_fio_root_create_select,
                .update = smoothaf_fio_create_update,
                .help = "Create a dedicated file in ML_CARD root; refuses an existing name.",
            },
            {
                .name = "Test File Paths",
                .select = smoothaf_fio_paths_select,
                .update = smoothaf_fio_paths_update,
                .help = "Read-only path comparison; no create, write, remove, append or seek.",
            },
            {
                .name = "Path Entry",
                .select = smoothaf_fio_path_entry_select,
                .update = smoothaf_fio_path_entry_update,
                .help = "Select one of the four paths for Path Result.",
            },
            {
                .name = "ML_CARD",
                .update = smoothaf_fio_ml_card_update,
                .help = "Logical Legacy ML card drive; selection is not changed.",
            },
            {
                .name = "SHOOTING_CARD",
                .update = smoothaf_fio_shooting_card_update,
                .help = "Logical Legacy shooting card drive; selection is not changed.",
            },
            {
                .name = "Path Result",
                .update = smoothaf_fio_path_result_update,
                .help = "Selected path: is_file, raw size return, size, read-only open and close.",
            },
            {
                .name = "Test Root Edit",
                .select = smoothaf_fio_root_edit_select,
                .update = smoothaf_fio_edit_update,
                .help = "Edit only the preallocated dedicated ML_CARD root file with O_RDWR|O_SYNC.",
            },
#endif
            {
                .name = "Log Page",
                .update = smoothaf_diag_page_update,
                .help = "Read-only page number in the fixed 32-record snapshot.",
            },
            {
                .name = "Log Entry",
                .update = smoothaf_diag_entry_update,
                .help = "Read-only diagnostic record; navigation never moves the lens.",
            },
            {
                .name = "Previous",
                .select = smoothaf_diag_previous_select,
                .help = "Show the previous diagnostic record.",
            },
            {
                .name = "Next",
                .select = smoothaf_diag_next_select,
                .help = "Show the next diagnostic record.",
            },
            {
                .name = "Clear History",
                .select = smoothaf_diag_clear_select,
                .help = "Clear records only when no SmoothAF task is active.",
            },
            MENU_EOL,
        },
    },
    {
        .name = "Focus Test",
        .select = smoothaf_search_select,
        .help = "Start one bounded hill-climbing search; LiveView only.",
    },
    {
        .name = "Canon AF Assist",
        .select = menu_open_submenu,
        .children = (struct menu_entry[])
        {
            {
                .name = "Status",
                .update = smoothaf_af_assist_update,
                .help = "Reserved; no safe Canon C API is available.",
            },
            {
                .name = "Trigger Canon AF",
                .select = smoothaf_af_assist_trigger_select,
                .help = "Blocked: no safe Canon C API; no lens movement.",
            },
            MENU_EOL,
        },
    },
    {
        .name = "AF Mode",
        .priv = &smoothaf_af_mode,
        .min = SMOOTHAF_HC_LEGACY,
        .max = SMOOTHAF_AF_NORMAL,
        .choices = CHOICES("Legacy HC", "Adaptive HC", "Normal AF"),
        .help = "Select single-shot focus policy; Legacy HC remains the default.",
    },
    {
        .name = "Debug Logging",
        .priv = &smoothaf_debug_logging,
        .max = 1,
        .help = "Enable fixed-size diagnostics logging; use Diagnostic Log Page/Entry to review it.",
    },
    {
        .name = "Log Count",
        .update = smoothaf_diag_page_update,
        .help = "Current retained diagnostic records; capacity is 32.",
    },
    {
        .name = "Log Failures",
        .priv = (void *)&smoothaf_log_publish_failures,
        .help = "Diagnostic events that could not acquire the state semaphore.",
    },
    {
        .name = "Sharpness Metric",
        .priv = &smoothaf_metric,
        .min = SMOOTHAF_METRIC_SAD,
        .max = SMOOTHAF_METRIC_LAPLACIAN,
        .choices = CHOICES("SAD", "Laplacian"),
    },
    {
        .name = "Step Size",
        .priv = &smoothaf_step,
        .min = 1,
        .max = 3,
        .unit = UNIT_DEC,
    },
    {
        .name = "Status",
        .update = smoothaf_status_update,
        .help = "Runtime status and last diagnostic values.",
    },
    {
        .name = "Stop",
        .select = smoothaf_stop_select,
        .help = "Cancel future movement; a synchronous lens command may finish first.",
    },
};

/* ---------------------------------------------------------------- */
/* Module lifecycle                                                  */
/* ---------------------------------------------------------------- */

static unsigned int smoothaf_init(void)
{
    int i;

    smoothaf_sem = create_named_semaphore("smoothaf_sem", 1);
    if (!smoothaf_sem)
    {
        return 1;
    }

    /* Enable is never persisted: always start disabled. */
    smoothaf_enabled = 0;
    smoothaf_cancel = 0;
    smoothaf_task_running = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 1;
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
    smoothaf_fio_shutdown = 0;
    smoothaf_fio_state_init(&smoothaf_fio_create_state, SMOOTHAF_FIO_ROOT_CREATE);
    smoothaf_fio_state_init(&smoothaf_fio_edit_state, SMOOTHAF_FIO_ROOT_EDIT);
    smoothaf_fio_state_init(&smoothaf_fio_paths_state, SMOOTHAF_FIO_FILE_PATHS);
    smoothaf_fio_path_entry = 0;
    for (i = 0; i < SMOOTHAF_FIO_PATH_COUNT; i++)
        smoothaf_fio_path_state_init(&smoothaf_fio_path_results[i]);
#endif
    smoothaf_status = SMOOTHAF_STATUS_IDLE;
    smoothaf_last_error = 0;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    smoothaf_last_lens_called = 0;
    smoothaf_last_lens_return = 0;
    smoothaf_last_score = 0;
    smoothaf_last_sad = 0;
    smoothaf_last_laplacian = 0;
    smoothaf_last_measure_valid = 0;
    smoothaf_monitor_init(&smoothaf_monitor, smoothaf_metric,
                          SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
    smoothaf_vlog_af_init(&smoothaf_vlog_af);
    smoothaf_vlog_af_mode = SMOOTHAF_VLOG_AF_OFF;
    smoothaf_af_move_attempts = 0;
    smoothaf_af_lens_calls = 0;
    smoothaf_af_move_successes = 0;
    smoothaf_af_commands_episode = 0;
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    smoothaf_af_initial_score = 0;
    smoothaf_af_post_move_score = 0;
    smoothaf_af_best_score = 0;
    smoothaf_af_score_delta = 0;
    smoothaf_af_best_position = 0;
    smoothaf_af_current_position = 0;
    smoothaf_af_last_direction = 1;
    smoothaf_monitor_task_requested = 0;
    smoothaf_monitor_deinit = 0;
    smoothaf_log_publish_failures = 0;
    smoothaf_last_measure_code = SMOOTHAF_MEASURE_WAITING;
    smoothaf_last_best = 0;
    smoothaf_last_mean = 0;
    smoothaf_last_time = 0;
    smoothaf_last_focus_pos = 0;
    smoothaf_last_commands = 0;
    smoothaf_last_reversals = 0;
    smoothaf_diag_count = 0;
    smoothaf_diag_page = 0;
    smoothaf_diag_seen_count = 0;
    smoothaf_session_started_ms = 0;
    smoothaf_session_finished_ms = 0;
#if SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS
    smoothaf_last_export_path[0] = 0;
    smoothaf_last_export_error = 0;
#endif
    smoothaf_log_init(&smoothaf_log);
    smoothaf_af_log_init(&smoothaf_af_log);
    smoothaf_af_assist_init(&smoothaf_af_assist);

    /* Clamp values loaded from an older config file. */
    if (smoothaf_metric < SMOOTHAF_METRIC_SAD ||
        smoothaf_metric > SMOOTHAF_METRIC_LAPLACIAN)
    {
        smoothaf_metric = SMOOTHAF_METRIC_SAD;
    }
    if (smoothaf_step < 1 || smoothaf_step > 3)
    {
        smoothaf_step = 1;
    }
    if (smoothaf_direction_choice < 0 || smoothaf_direction_choice > 1)
    {
        smoothaf_direction_choice = 0;
    }
    if (smoothaf_hc_mode < SMOOTHAF_HC_LEGACY ||
        smoothaf_hc_mode > SMOOTHAF_HC_ADAPTIVE)
    {
        smoothaf_hc_mode = SMOOTHAF_HC_LEGACY;
    }
    if (smoothaf_af_mode < SMOOTHAF_HC_LEGACY ||
        smoothaf_af_mode > SMOOTHAF_AF_NORMAL)
    {
        smoothaf_af_mode = (smoothaf_hc_mode == SMOOTHAF_HC_ADAPTIVE)
                         ? SMOOTHAF_HC_ADAPTIVE : SMOOTHAF_HC_LEGACY;
    }
    if (smoothaf_debug_logging < 0 || smoothaf_debug_logging > 1)
    {
        smoothaf_debug_logging = 0;
    }
    smoothaf_monitor_interval = smoothaf_monitor_normalize_interval(
        smoothaf_monitor_interval);
    smoothaf_monitor.interval_index = smoothaf_monitor_interval;
    smoothaf_log_init(&smoothaf_log);

    /* No lens movement on load. */
    menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));
    return 0;
}

static unsigned int smoothaf_deinit(void)
{
    int i;

    smoothaf_fio_shutdown = 1;
    smoothaf_monitor_deinit = 1;
    smoothaf_task_stop_requested = 1;
    smoothaf_monitor_stop(&smoothaf_monitor);
    if (smoothaf_lock())
    {
        smoothaf_enabled = 0;
        smoothaf_cancel = 1;
        smoothaf_unlock();
    }
    else
    {
        /* Conservative fallback: stop accepting work even if the lock is
         * unavailable during shutdown. */
        smoothaf_enabled = 0;
        smoothaf_cancel = 1;
    }

    /* lens_focus(wait=1) cannot be interrupted.  Wait for the task to observe
     * cancellation, but do not block shutdown forever.  The Legacy loader
     * calls deinit() but provides no task join/kill guarantee; a late task
     * therefore remains an unload hazard after this bounded wait. */
    for (i = 0; i < SMOOTHAF_DEINIT_WAIT_MS / 10 &&
         (smoothaf_is_task_running() || !smoothaf_task_exited); i++)
    {
        msleep(10);
    }

    /* The Legacy loader ignores this return value and does not provide a task
     * join/kill API.  A task still running after the bounded wait therefore
     * remains an unresolved unload hazard; do not claim that this is safe. */
    if (smoothaf_is_task_running() || !smoothaf_task_exited)
    {
        smoothaf_last_error = 1;
        smoothaf_last_block = SMOOTHAF_BLOCK_CANCELLED;
    }

    menu_remove("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(smoothaf_init)
    MODULE_DEINIT(smoothaf_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(smoothaf_metric)
    MODULE_CONFIG(smoothaf_step)
    MODULE_CONFIG(smoothaf_direction_choice)
    MODULE_CONFIG(smoothaf_hc_mode)
    MODULE_CONFIG(smoothaf_debug_logging)
    MODULE_CONFIG(smoothaf_monitor_interval)
MODULE_CONFIGS_END()
