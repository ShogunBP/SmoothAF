/*
 * SmoothAF 0.8-pre-refactor - bounded single-shot contrast focus for Magic Lantern.
 *
 * The algorithm and camera guards remain allocation-free and task-bounded. Runtime
 * diagnostics are one fixed RAM telemetry record; no history, file export, or
 * filesystem probing is performed.
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
#include "smoothaf_episode.h"
#include "smoothaf_algorithm.h"
#include "smoothaf_direction.h"
#include "smoothaf_recovery.h"
#include "smoothaf_diagnostics.h"
#include "smoothaf_build_id.h"
#include "smoothaf_logger.h"
#include "smoothaf_logger_fio.h"

#define SMOOTHAF_HC_LEGACY       0
#define SMOOTHAF_HC_ADAPTIVE     1
#define SMOOTHAF_AF_NORMAL       2
#define SMOOTHAF_TEST_DIRECTION_AUTO 0
#define SMOOTHAF_TEST_DIRECTION_A    1
#define SMOOTHAF_TEST_DIRECTION_B    2

#define SMOOTHAF_SETTLE_MS       120
#define SMOOTHAF_COOLDOWN_MS     80
#define SMOOTHAF_TIMEOUT_MS      5000
#define SMOOTHAF_TASK_PRIORITY   0x1c
#define SMOOTHAF_TASK_STACK      0x1000
#define SMOOTHAF_DEINIT_WAIT_MS  3000

#define SMOOTHAF_TASK_MEASURE    1
#define SMOOTHAF_TASK_STEP       2
#define SMOOTHAF_TASK_SEARCH     3
#define SMOOTHAF_TASK_MONITOR    4
#define SMOOTHAF_TASK_FIO        5

#define SMOOTHAF_FIO_TASK_PRIORITY 0x1d
#define SMOOTHAF_FIO_TASK_STACK    0x1000

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

#define SMOOTHAF_MOVEMENT_IDLE        0
#define SMOOTHAF_MOVEMENT_MOVING      1
#define SMOOTHAF_MOVEMENT_COMPLETE    2
#define SMOOTHAF_MOVEMENT_FAILED      3

#define SMOOTHAF_BLOCK_NONE           0
#define SMOOTHAF_BLOCK_DISABLED       1
#define SMOOTHAF_BLOCK_CANCELLED      2
#define SMOOTHAF_BLOCK_NO_LV          3
#define SMOOTHAF_BLOCK_MOVIE         4
#define SMOOTHAF_BLOCK_RECORDING      5
#define SMOOTHAF_BLOCK_MANUAL_FOCUS   6
#define SMOOTHAF_BLOCK_CONTINUOUS_AF  7
#define SMOOTHAF_BLOCK_FOLLOW_FOCUS   8
#define SMOOTHAF_BLOCK_RACK_FOCUS     9
#define SMOOTHAF_BLOCK_FOCUS_STACK   10
#define SMOOTHAF_BLOCK_PARAMS        11
#define SMOOTHAF_BLOCK_SEMAPHORE     12
#define SMOOTHAF_BLOCK_TASK_CREATE   13
#define SMOOTHAF_BLOCK_FRAME         14
#define SMOOTHAF_BLOCK_BUSY          15

/* Legacy keys are retained for saved-configuration compatibility. */
static CONFIG_INT("smoothaf.metric", smoothaf_metric, SMOOTHAF_METRIC_SAD);
static CONFIG_INT("smoothaf.test_metric", smoothaf_test_metric, SMOOTHAF_METRIC_SAD);
static CONFIG_INT("smoothaf.vlog_metric", smoothaf_vlog_metric, SMOOTHAF_METRIC_SAD);
static CONFIG_INT("smoothaf.step", smoothaf_step, 3);
static CONFIG_INT("smoothaf.test_step", smoothaf_test_step, 3);
static CONFIG_INT("smoothaf.vlog_step", smoothaf_vlog_step, 3);
/* Diagnostic Direction is reserved for explicit diagnostic movement.  Vlog AF
 * has its own persisted initial direction so changing the diagnostic control
 * cannot bias an already armed or newly automatic episode. */
static CONFIG_INT("smoothaf.dir", smoothaf_direction_choice, 0);
static CONFIG_INT("smoothaf.af_dir", smoothaf_af_direction_choice, 0);
/* -1 distinguishes an untouched pre-migration key from an explicitly saved
 * AUTO value, so legacy AF Direction cannot overwrite the latter. */
static CONFIG_INT("smoothaf.vlog_direction", smoothaf_vlog_direction, -1);
static CONFIG_INT("smoothaf.test_direction", smoothaf_test_direction,
                  SMOOTHAF_TEST_DIRECTION_AUTO);
static CONFIG_INT("smoothaf.hc_mode", smoothaf_hc_mode, SMOOTHAF_HC_LEGACY);
static CONFIG_INT("smoothaf.af_mode", smoothaf_af_mode, -1);
static CONFIG_INT("smoothaf.vlog_algorithm", smoothaf_vlog_algorithm,
                  SMOOTHAF_ALGORITHM_CURRENT_VLOG);
static CONFIG_INT("smoothaf.vlog_algorithm_pending", smoothaf_vlog_algorithm_pending,
                  SMOOTHAF_ALGORITHM_CURRENT_VLOG);
static CONFIG_INT("smoothaf.monitor_interval", smoothaf_monitor_interval,
                 SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
static CONFIG_INT("smoothaf.vlog_episode_limit", smoothaf_vlog_episode_limit,
                 SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT);
static CONFIG_INT("smoothaf.far_focus_recovery", smoothaf_far_focus_recovery, 0);

/* Persistent Logger (Phase 3A) - disabled by default */
static CONFIG_INT("smoothaf.logger_enabled", smoothaf_logger_enabled, 0);
static CONFIG_INT("smoothaf.logger_fio_enabled", smoothaf_logger_fio_enabled, 0);
static CONFIG_INT("smoothaf.logger_max_file_kb", smoothaf_logger_max_file_kb, 256);
static CONFIG_INT("smoothaf.logger_max_files", smoothaf_logger_max_files, 9);

/* Deliberately volatile and default-off: authorization never arms an episode. */
static CONFIG_INT("smoothaf.movie_active_experimental",
                  smoothaf_movie_active_experimental, 0);

static int smoothaf_enabled;
static volatile int smoothaf_panic;
static volatile int smoothaf_status = SMOOTHAF_STATUS_IDLE;
static volatile int smoothaf_cancel;
static volatile int smoothaf_task_running;
static volatile int smoothaf_task_stop_requested;
static volatile int smoothaf_task_exited = 1;
static volatile int smoothaf_last_score;
static volatile int smoothaf_last_sad;
static volatile int smoothaf_last_laplacian;
static volatile int smoothaf_last_measure_valid;
static volatile int smoothaf_last_best;
static volatile int smoothaf_last_mean;
static volatile int smoothaf_last_time;
static volatile int smoothaf_last_focus_pos;
static volatile int smoothaf_last_algorithm_focus_pos;
static volatile int smoothaf_last_error;
static volatile int smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
static volatile int smoothaf_last_commands;
static volatile int smoothaf_last_reversals;
static volatile int smoothaf_last_lens_called;
static volatile int smoothaf_last_lens_return;
static volatile int smoothaf_last_measure_code;
static volatile int smoothaf_last_mode = SMOOTHAF_HC_LEGACY;
static volatile int smoothaf_last_phase;
static volatile int smoothaf_last_algorithm_valid;
static volatile int smoothaf_last_algorithm_status;
static volatile int smoothaf_test_session;
static volatile int smoothaf_vlog_algorithm_active = SMOOTHAF_ALGORITHM_CURRENT_VLOG;
static volatile int smoothaf_vlog_algorithm_pending_flag;
static volatile int smoothaf_vlog_algorithm_episode;
static volatile int smoothaf_test_auto_result;
static volatile int smoothaf_test_auto_commands;
static volatile int smoothaf_test_auto_reversals;
static volatile int smoothaf_test_auto_first_score;
static volatile int smoothaf_test_auto_opposite_score;
static volatile int smoothaf_test_auto_first_delta;
static volatile int smoothaf_test_auto_opposite_delta;
static volatile int smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_IDLE;
static volatile int smoothaf_af_move_attempts;
static volatile int smoothaf_af_lens_calls;
static volatile int smoothaf_af_move_successes;
static volatile int smoothaf_af_commands_episode;
static volatile int smoothaf_af_initial_score;
static volatile int smoothaf_af_post_move_score;
static volatile int smoothaf_af_best_score;
static volatile int smoothaf_af_score_delta;
static volatile int smoothaf_af_best_position;
static volatile int smoothaf_af_current_position;
static volatile int smoothaf_af_last_direction = 1;
static volatile int smoothaf_af_command_block;
static volatile int smoothaf_af_episode_state_before_error;
static volatile int smoothaf_af_episode_reason;
static volatile int smoothaf_af_post_move_valid;
static volatile int smoothaf_af_last_result_valid;
static volatile int smoothaf_af_confidence;
static volatile int smoothaf_af_decision_relation;
static volatile int smoothaf_af_episode_complete;
static volatile int smoothaf_af_episode_block_reason;
static volatile int smoothaf_af_episode_cancel_reason;
static volatile int smoothaf_af_episode_lens_return;
static volatile int smoothaf_af_episode_summary_pending;
static volatile uint32_t smoothaf_af_episode_id;
static volatile int smoothaf_measure_requested;
static volatile int smoothaf_persist_session_active;
static volatile int smoothaf_fio_task_running;
static volatile int smoothaf_fio_task_exited = 1;
static volatile int smoothaf_fio_task_created;
static volatile int smoothaf_fio_shutdown_requested;
static volatile uint32_t smoothaf_fio_seen_errors;
static volatile uint32_t smoothaf_fio_seen_partials;
static volatile uint32_t smoothaf_persist_session_episodes;
static uint32_t smoothaf_session_started_ms;
static uint32_t smoothaf_session_finished_ms;
static struct smoothaf_telemetry smoothaf_telemetry;
static struct smoothaf_log_buffer smoothaf_log;
static struct smoothaf_episode_log smoothaf_episode_log;

/* Persistent Logger (Phase 3A) */
static struct smoothaf_persist_log_buffer smoothaf_persist_log;
static struct smoothaf_log_fio_backend smoothaf_persist_fio;
static struct smoothaf_persist_log_batch smoothaf_persist_batch;
/* Note: smoothaf_logger_enabled, smoothaf_logger_fio_enabled, etc. are defined via CONFIG_INT above */

static void smoothaf_persist_logger_record_fio_errors(void)
{
    uint32_t errors = smoothaf_log_fio_get_write_errors(&smoothaf_persist_fio);
    uint32_t partials = smoothaf_log_fio_get_partial_writes(&smoothaf_persist_fio);
    if (!smoothaf_persist_logger_is_enabled(&smoothaf_persist_log)) {
        smoothaf_fio_seen_errors = errors;
        smoothaf_fio_seen_partials = partials;
        return;
    }
    if (errors != smoothaf_fio_seen_errors || partials != smoothaf_fio_seen_partials) {
        smoothaf_persist_log_write_error(
            &smoothaf_persist_log, (uint32_t)get_ms_clock(),
            (uint16_t)smoothaf_af_episode_id,
            smoothaf_persist_log_get_dropped(&smoothaf_persist_log),
            smoothaf_log_fio_get_last_error(&smoothaf_persist_fio));
        smoothaf_fio_seen_errors = errors;
        smoothaf_fio_seen_partials = partials;
    }
}

static void smoothaf_persist_logger_sync_config(void);

static void smoothaf_persist_logger_sync_config(void)
{
    bool logger_enabled = smoothaf_logger_enabled ? true : false;
    bool fio_enabled = (smoothaf_logger_fio_enabled && logger_enabled) ? true : false;

    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log) != logger_enabled)
        smoothaf_persist_logger_set_enabled(&smoothaf_persist_log, logger_enabled);

    smoothaf_log_fio_request_enabled(&smoothaf_persist_fio, fio_enabled);
    smoothaf_log_fio_request_limits(&smoothaf_persist_fio,
                                    smoothaf_logger_max_file_kb > 0
                                        ? (uint32_t)smoothaf_logger_max_file_kb * 1024
                                        : 0,
                                    smoothaf_logger_max_files > 0 &&
                                    smoothaf_logger_max_files <= SMOOTHAF_LOG_FIO_MAX_FILES
                                        ? (uint32_t)smoothaf_logger_max_files : 0);
}
static struct smoothaf_episode_summary smoothaf_episode_summary_snapshot[SMOOTHAF_EPISODE_SUMMARY_CAPACITY];
static struct smoothaf_log_entry smoothaf_diag_snapshot[SMOOTHAF_DIAG_CAPACITY];
static size_t smoothaf_diag_count;
static size_t smoothaf_diag_page;
static size_t smoothaf_diag_seen_total;
static size_t smoothaf_episode_summary_count;
static size_t smoothaf_episode_summary_page;
static size_t smoothaf_episode_summary_seen_count;
static struct smoothaf_monitor smoothaf_monitor;
static struct smoothaf_vlog_af smoothaf_vlog_af;
static volatile int smoothaf_vlog_af_mode = SMOOTHAF_VLOG_AF_OFF;
static volatile int smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
static volatile int smoothaf_monitor_task_requested;
static volatile int smoothaf_monitor_deinit;
static struct smoothaf_movie_recording_state smoothaf_movie_recording;
static volatile int smoothaf_movie_automatic_session;
static volatile int smoothaf_movie_start_pending;
static struct semaphore *smoothaf_sem;
static int smoothaf_search_direction = -1;

#define SMOOTHAF_MEASURE_OK       0
#define SMOOTHAF_MEASURE_LOW      1
#define SMOOTHAF_MEASURE_ERROR    2
#define SMOOTHAF_MEASURE_WAITING  3

static int smoothaf_is_task_running(void);
static int smoothaf_start_fio_task(void);
static void smoothaf_measure_task(void);
static void smoothaf_cancel_pending_measure(void);
static int smoothaf_start_task(int task_id, int requires_enable);
static const char *smoothaf_movement_state_name(void);
static void smoothaf_vlog_algorithm_apply_at_episode_boundary(void);
static int smoothaf_vlog_direction_auto_selected(void);

static const char *smoothaf_status_name_for(int status)
{
    switch (status)
    {
        case SMOOTHAF_STATUS_MEASURING: return "Measuring";
        case SMOOTHAF_STATUS_MOVING: return "Moving";
        case SMOOTHAF_STATUS_LOCKED: return "Focus Locked";
        case SMOOTHAF_STATUS_SEARCH_LIMIT: return "Search Limit";
        case SMOOTHAF_STATUS_NO_CONFIDENCE: return "No Confidence";
        case SMOOTHAF_STATUS_CANCELLED: return "Cancelled";
        case SMOOTHAF_STATUS_TIMEOUT: return "Timeout";
        case SMOOTHAF_STATUS_LENS_ERROR: return "Lens Error";
        case SMOOTHAF_STATUS_ERROR: return "Error";
        default: return "Idle";
    }
}

static const char *smoothaf_status_name(void)
{
    return smoothaf_status_name_for(smoothaf_status);
}

static const char *smoothaf_block_name(int reason)
{
    switch (reason)
    {
        case SMOOTHAF_BLOCK_DISABLED: return "Disabled";
        case SMOOTHAF_BLOCK_CANCELLED: return "Stopped";
        case SMOOTHAF_BLOCK_NO_LV: return "No LiveView";
        case SMOOTHAF_BLOCK_MOVIE: return "Movie mode";
        case SMOOTHAF_BLOCK_RECORDING: return "Recording";
        case SMOOTHAF_BLOCK_MANUAL_FOCUS: return "Manual focus";
        case SMOOTHAF_BLOCK_CONTINUOUS_AF: return "Continuous AF";
        case SMOOTHAF_BLOCK_FOLLOW_FOCUS: return "Follow Focus";
        case SMOOTHAF_BLOCK_RACK_FOCUS: return "Rack Focus";
        case SMOOTHAF_BLOCK_FOCUS_STACK: return "Focus Stacking";
        case SMOOTHAF_BLOCK_PARAMS: return "Invalid parameters";
        case SMOOTHAF_BLOCK_SEMAPHORE: return "Semaphore unavailable";
        case SMOOTHAF_BLOCK_TASK_CREATE: return "Task creation failed";
        case SMOOTHAF_BLOCK_FRAME: return "Invalid frame";
        case SMOOTHAF_BLOCK_BUSY: return "Task busy";
        default: return "None";
    }
}

static void smoothaf_telemetry_update(int score, int duration, int direction,
                                      int step, int state, int error, int reason)
{
    smoothaf_telemetry.score = score;
    smoothaf_telemetry.duration_ms = (uint32_t)duration;
    smoothaf_telemetry.direction = direction;
    smoothaf_telemetry.step = step;
    smoothaf_telemetry.state = state;
    smoothaf_telemetry.error = error;
    smoothaf_telemetry.reason = reason;
}

static int smoothaf_lock(void)
{
    return smoothaf_sem && take_semaphore(smoothaf_sem, 50) == 0;
}

static void smoothaf_unlock(void)
{
    if (smoothaf_sem) give_semaphore(smoothaf_sem);
}

static void smoothaf_log_event_extended(smoothaf_log_type_t type,
                                        int32_t v1, int32_t v2, int32_t v3,
                                        int32_t v4, int32_t v5)
{
    if (!smoothaf_lock()) return;
    smoothaf_log.tick = (uint32_t)get_ms_clock();
    smoothaf_log_write_extended(&smoothaf_log, type, v1, v2, v3, v4, v5);
    smoothaf_unlock();
}

static void smoothaf_log_event(smoothaf_log_type_t type,
                               int32_t v1, int32_t v2, int32_t v3)
{
    smoothaf_log_event_extended(type, v1, v2, v3, 0, 0);
}

static void smoothaf_episode_event(smoothaf_episode_event_type_t type,
                                   int result, int score, int best_score,
                                   int confidence, int direction, int step,
                                   int position, uint32_t started_ms,
                                   uint32_t ended_ms)
{
    struct smoothaf_episode_event *event;
    if (!smoothaf_af_episode_id && type != SMOOTHAF_EPISODE_EVENT_ARM) return;
    if (smoothaf_lock())
    {
        smoothaf_episode_log_event(&smoothaf_episode_log, (uint32_t)get_ms_clock(),
                               smoothaf_af_episode_id, type, result, score,
                               best_score, confidence, direction, step, position,
                               smoothaf_vlog_af.best_position,
                               smoothaf_af_episode_lens_return,
                               smoothaf_diag_elapsed(started_ms, ended_ms));
        event = &smoothaf_episode_log.events[
            (smoothaf_episode_log.event_head + SMOOTHAF_EPISODE_EVENT_CAPACITY - 1) %
            SMOOTHAF_EPISODE_EVENT_CAPACITY];
        event->relation = smoothaf_af_decision_relation;
        smoothaf_unlock();
    }
}

static int smoothaf_episode_result_map(int result)
{
    switch (result)
    {
        case SMOOTHAF_VLOG_AF_RESULT_IMPROVED:
            return SMOOTHAF_EPISODE_RESULT_IMPROVED;
        case SMOOTHAF_VLOG_AF_RESULT_WORSE:
            return SMOOTHAF_EPISODE_RESULT_WORSE;
        case SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN:
            return SMOOTHAF_EPISODE_RESULT_UNCERTAIN;
        case SMOOTHAF_VLOG_AF_RESULT_CONVERGED:
            return SMOOTHAF_EPISODE_RESULT_CONVERGED;
        case SMOOTHAF_VLOG_AF_RESULT_TIMEOUT:
            return SMOOTHAF_EPISODE_RESULT_TIMEOUT;
        case SMOOTHAF_VLOG_AF_RESULT_BLOCKED:
            return SMOOTHAF_EPISODE_RESULT_BLOCKED;
        case SMOOTHAF_VLOG_AF_RESULT_FAILED:
            return SMOOTHAF_EPISODE_RESULT_LENS_FAILED;
        case SMOOTHAF_VLOG_AF_RESULT_CANCELLED:
            return SMOOTHAF_EPISODE_RESULT_CANCELLED;
        case SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT:
            return SMOOTHAF_EPISODE_RESULT_NO_MOVEMENT;
        case SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT:
            return SMOOTHAF_EPISODE_RESULT_NO_IMPROVEMENT;
        case SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT:
            return SMOOTHAF_EPISODE_RESULT_SEARCH_LIMIT;
        case SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED:
            return SMOOTHAF_EPISODE_RESULT_BEST_RECOVERED;
        case SMOOTHAF_VLOG_AF_RESULT_BEST_NOT_RECOVERED:
            return SMOOTHAF_EPISODE_RESULT_BEST_NOT_RECOVERED;
        case SMOOTHAF_VLOG_AF_RESULT_DRY_RUN:
            return SMOOTHAF_EPISODE_RESULT_DRY_RUN;
        default:
            return SMOOTHAF_EPISODE_RESULT_NONE;
    }
}

static void smoothaf_episode_summary_publish(int result, uint32_t ended_ms)
{
    struct smoothaf_episode_summary summary;
    if (!smoothaf_af_episode_id) return;
    if (!smoothaf_lock())
    {
        smoothaf_af_episode_summary_pending = 1;
        return;
    }
    if (smoothaf_af_episode_complete)
    {
        smoothaf_unlock();
        return;
    }
    smoothaf_af_episode_complete = 1;
    summary.episode_id = smoothaf_af_episode_id;
    summary.started_ms = smoothaf_vlog_af.started_ms;
    summary.ended_ms = ended_ms;
    summary.image_ms = smoothaf_vlog_af.image_ms;
    summary.wait_ms = smoothaf_vlog_af.wait_ms;
    summary.movement_ms = smoothaf_vlog_af.movement_ms;
    summary.stabilization_ms = smoothaf_vlog_af.stabilization_ms;
    summary.total_lens_ms = smoothaf_vlog_af.total_lens_ms;
    summary.total_settle_ms = smoothaf_vlog_af.total_settle_ms;
    summary.total_sample_wait_ms = smoothaf_vlog_af.total_sample_wait_ms;
    summary.commands = smoothaf_vlog_af.commands;
    summary.reversals = smoothaf_vlog_af.reversals;
    summary.initial_score = smoothaf_vlog_af.initial_score;
    summary.best_score = smoothaf_vlog_af.best;
    summary.final_score = smoothaf_af_post_move_valid
                        ? smoothaf_vlog_af.post_move_score : -1;
    summary.best_position = smoothaf_vlog_af.commands
                          ? smoothaf_vlog_af.best_position : -1;
    summary.final_position = smoothaf_vlog_af.commands
                           ? smoothaf_vlog_af.current_position : -1;
    summary.result = (int16_t)result;
    summary.lens_calls = (int16_t)smoothaf_vlog_af.lens_calls;
    summary.mode = (int16_t)smoothaf_vlog_af.mode;
    summary.block_reason = (int16_t)smoothaf_af_episode_block_reason;
    summary.cancel_reason = (int16_t)smoothaf_af_episode_cancel_reason;
    summary.lens_return = (int16_t)smoothaf_af_episode_lens_return;
    summary.post_move_valid = (int16_t)smoothaf_af_post_move_valid;
    summary.confidence = (int16_t)smoothaf_af_confidence;
    summary.complete = 1;
    if (smoothaf_persist_session_episodes != UINT32_MAX)
        smoothaf_persist_session_episodes++;
    smoothaf_episode_log_summary(&smoothaf_episode_log, &summary);
    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
        smoothaf_persist_log_episode_end(&smoothaf_persist_log, ended_ms,
                                         summary.episode_id,
                                         (uint8_t)smoothaf_episode_result_map(result),
                                         summary.best_score, summary.best_position,
                                         summary.final_score, summary.final_position,
                                         (uint8_t)summary.commands);
    smoothaf_episode_log_event(&smoothaf_episode_log, ended_ms, summary.episode_id,
                               SMOOTHAF_EPISODE_EVENT_COMPLETE, result,
                               summary.final_score, summary.best_score,
                               summary.confidence, smoothaf_vlog_af.last_direction,
                               smoothaf_vlog_af.command_step, summary.final_position,
                               summary.best_position, summary.lens_return,
                               smoothaf_diag_elapsed(summary.started_ms, ended_ms));
    smoothaf_episode_log.events[(smoothaf_episode_log.event_head +
        SMOOTHAF_EPISODE_EVENT_CAPACITY - 1) %
        SMOOTHAF_EPISODE_EVENT_CAPACITY].relation = smoothaf_af_decision_relation;
    smoothaf_af_episode_summary_pending = 0;
    smoothaf_unlock();
}

static void smoothaf_episode_begin(smoothaf_episode_event_type_t type)
{
    uint32_t now = (uint32_t)get_ms_clock();
    if (++smoothaf_af_episode_id == 0) smoothaf_af_episode_id = 1;
    smoothaf_af_episode_complete = 0;
    smoothaf_af_episode_block_reason = 0;
    smoothaf_af_episode_cancel_reason = 0;
    smoothaf_af_episode_lens_return = 0;
    smoothaf_af_post_move_valid = 0;
    smoothaf_af_last_result_valid = 0;
    smoothaf_af_decision_relation = 0;
    smoothaf_vlog_af.started_ms = now;
    smoothaf_episode_event(type, SMOOTHAF_EPISODE_RESULT_NONE,
                           smoothaf_vlog_af.initial_score,
                           smoothaf_vlog_af.best, smoothaf_af_confidence,
                           smoothaf_vlog_af.direction,
                           smoothaf_vlog_af.command_step,
                           smoothaf_vlog_af.current_position, now, now);

    /* Persistent Logger: EPISODE_START hook (non-blocking producer) */
    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
    {
        smoothaf_persist_log_episode_start(&smoothaf_persist_log, now,
                                   (uint16_t)smoothaf_af_episode_id,
                                   (uint8_t)smoothaf_vlog_af.algorithm,
                                   smoothaf_vlog_af.initial_score);
    }
}

static void smoothaf_persist_session_start(void)
{
    if (!smoothaf_persist_logger_is_enabled(&smoothaf_persist_log) ||
        smoothaf_persist_session_active)
        return;
    if (smoothaf_persist_log_session_start(
            &smoothaf_persist_log, (uint32_t)get_ms_clock(),
            smoothaf_build_id_value(), SMOOTHAF_VLOG_AF_TIMEOUT_MS,
            (uint8_t)smoothaf_vlog_episode_limit,
            (uint8_t)smoothaf_vlog_direction_auto_selected()))
    {
        smoothaf_persist_session_episodes = 0;
        smoothaf_persist_session_active = 1;
    }
}

static void smoothaf_persist_session_end(uint8_t reason)
{
    if (!smoothaf_persist_session_active ||
        !smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
        return;
    smoothaf_persist_log_session_end(
        &smoothaf_persist_log, (uint32_t)get_ms_clock(), reason,
        (uint32_t)get_ms_clock() - smoothaf_session_started_ms,
        (uint16_t)(smoothaf_persist_session_episodes > UINT16_MAX
                   ? UINT16_MAX : smoothaf_persist_session_episodes));
    smoothaf_persist_session_active = 0;
}

static int smoothaf_episode_cancel_requested(void)
{
    return smoothaf_cancel || smoothaf_panic || smoothaf_task_stop_requested ||
           smoothaf_vlog_af_should_cancel(&smoothaf_vlog_af,
                                          smoothaf_vlog_af_mode) ||
           ((smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE ||
             smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) &&
            !smoothaf_enabled);
}

static void smoothaf_episode_stop(void)
{
    uint32_t ended_ms = (uint32_t)get_ms_clock();
    int cancelled = smoothaf_episode_cancel_requested() ||
                    smoothaf_vlog_af.episode_result ==
                    SMOOTHAF_VLOG_AF_RESULT_CANCELLED;
    int publish_pending = !smoothaf_af_episode_complete;
    if (cancelled && smoothaf_vlog_af.armed)
        smoothaf_vlog_af.episode_result = SMOOTHAF_VLOG_AF_RESULT_CANCELLED;
    smoothaf_vlog_af_stop(&smoothaf_vlog_af);
    smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
    smoothaf_af_episode_reason = smoothaf_vlog_af.episode_result;
    smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
    if (smoothaf_vlog_af.episode_result == SMOOTHAF_VLOG_AF_RESULT_CANCELLED)
    {
        if (!smoothaf_last_block)
            smoothaf_last_block = SMOOTHAF_BLOCK_CANCELLED;
        smoothaf_af_episode_cancel_reason = smoothaf_last_block;
    }
    smoothaf_af_episode_state_before_error = smoothaf_vlog_af.state;
    smoothaf_episode_summary_publish(
        smoothaf_episode_result_map(smoothaf_vlog_af.episode_result), ended_ms);
    if (publish_pending &&
        smoothaf_vlog_af.episode_result == SMOOTHAF_VLOG_AF_RESULT_CANCELLED &&
        smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
        smoothaf_persist_log_cancelled(&smoothaf_persist_log, ended_ms,
                                       (uint16_t)smoothaf_af_episode_id,
                                       smoothaf_panic ? 2 :
                                       (smoothaf_task_stop_requested ? 1 :
                                        (smoothaf_last_block == SMOOTHAF_BLOCK_DISABLED
                                         ? 3 : 4)),
                                       (uint8_t)smoothaf_vlog_af.commands);
}

static void smoothaf_episode_cancel(void)
{
    smoothaf_vlog_af.episode_result = SMOOTHAF_VLOG_AF_RESULT_CANCELLED;
    smoothaf_episode_stop();
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_CANCELLED;
}

/* Menu/recording callbacks request cancellation while the monitor task owns
 * the episode.  The worker performs the final snapshot after its loop exits. */
static void smoothaf_request_episode_stop(void)
{
    if (smoothaf_is_task_running())
    {
        smoothaf_cancel = 1;
        smoothaf_task_stop_requested = 1;
        smoothaf_monitor_stop(&smoothaf_monitor);
        return;
    }
    smoothaf_episode_stop();
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

static void smoothaf_diag_refresh(void)
{
    size_t total;
    if (!smoothaf_lock()) return;
    smoothaf_diag_count = smoothaf_log_snapshot(&smoothaf_log,
                                                smoothaf_diag_snapshot,
                                                SMOOTHAF_DIAG_CAPACITY);
    total = smoothaf_log_total(&smoothaf_log);
    smoothaf_diag_page = smoothaf_diag_page_after_refresh(
        smoothaf_diag_count, total, smoothaf_diag_page,
        &smoothaf_diag_seen_total);
    smoothaf_unlock();
}

static int smoothaf_direction_value(void)
{
    return smoothaf_direction_choice ? 1 : -1;
}

static int smoothaf_af_direction_value(void)
{
    return smoothaf_af_direction_choice ? 1 : -1;
}

static int smoothaf_vlog_direction_value(void)
{
    return smoothaf_vlog_direction_forced_sign(smoothaf_vlog_direction);
}

static int smoothaf_vlog_direction_auto_selected(void)
{
    return smoothaf_vlog_direction_is_auto(smoothaf_vlog_direction);
}

static int smoothaf_vlog_direction_for_episode(void)
{
    return smoothaf_vlog_direction_auto_selected()
         ? -1 : smoothaf_vlog_direction_value();
}

static int smoothaf_vlog_probe_step(void)
{
    return smoothaf_vlog_step >= 1 && smoothaf_vlog_step <= 3
         ? smoothaf_vlog_step : 3;
}

static int smoothaf_test_direction_value(void)
{
    return smoothaf_test_direction == SMOOTHAF_TEST_DIRECTION_B ? 1 : -1;
}

static int smoothaf_active_metric(void)
{
    return smoothaf_test_session ? smoothaf_test_metric : smoothaf_vlog_metric;
}

static int smoothaf_vlog_algorithm_value(void)
{
    int selected = smoothaf_algorithm_normalize(smoothaf_vlog_algorithm);
    if (smoothaf_vlog_algorithm != selected)
        smoothaf_vlog_algorithm = SMOOTHAF_ALGORITHM_CURRENT_VLOG;
    return selected;
}

static const char *smoothaf_vlog_algorithm_name(void)
{
    return smoothaf_algorithm_name_for(
        smoothaf_vlog_algorithm_pending_flag
        ? smoothaf_vlog_algorithm_pending : smoothaf_vlog_algorithm_active);
}


static void smoothaf_monitor_update_from_measurement(int score, int mean, int sad,
                                                       int laplacian, int valid,
                                                       uint32_t duration)
{
    struct smoothaf_monitor_sample sample;
    int from;
    int to;
    if (!smoothaf_monitor.enabled) return;
    sample.score = score;
    sample.mean = mean;
    sample.sad = sad;
    sample.laplacian = laplacian;
    sample.valid = valid;
    sample.confident = valid && smoothaf_has_confidence(score, mean);
    sample.metric = smoothaf_active_metric() == SMOOTHAF_METRIC_LAPLACIAN
                  ? SMOOTHAF_MONITOR_METRIC_LAPLACIAN
                  : SMOOTHAF_MONITOR_METRIC_SAD;
    sample.duration_ms = duration;
    smoothaf_monitor_accept(&smoothaf_monitor, &sample, (uint32_t)get_ms_clock());
    if (smoothaf_monitor_take_transition(&smoothaf_monitor, &from, &to))
    {
        smoothaf_telemetry_update(score, (int)duration, 0, smoothaf_vlog_step,
                                  to, valid ? 0 : 1, valid ? 0 : SMOOTHAF_BLOCK_FRAME);
        smoothaf_log_event_extended(SMOOTHAF_LOG_MONITOR, from, to,
                                    smoothaf_monitor.reference,
                                    smoothaf_monitor.smoothed,
                                    smoothaf_monitor.metrics.valid);
    }
}

static void smoothaf_publish_measurement(int score, int mean, int sad, int laplacian,
                                         int valid, int code)
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
        if (smoothaf_last_block == SMOOTHAF_BLOCK_NONE)
            smoothaf_last_block = SMOOTHAF_BLOCK_FRAME;
        smoothaf_monitor_update_from_measurement(-1, mean, sad, laplacian,
                                                 0, 0);
        smoothaf_telemetry_update(-1, 0, 0, smoothaf_test_session ? smoothaf_test_step : smoothaf_vlog_step, smoothaf_status,
                                  1, smoothaf_last_block);
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR, code, mean, sad,
                                    laplacian, 0);
        return;
    }
    smoothaf_last_score = score;
    smoothaf_last_best = score;
    smoothaf_monitor_update_from_measurement(score, mean, sad, laplacian,
                                             1, 0);
    smoothaf_telemetry_update(score, 0, 0, smoothaf_test_session ? smoothaf_test_step : smoothaf_vlog_step, smoothaf_status,
                              0, SMOOTHAF_BLOCK_NONE);
    smoothaf_log_event_extended(SMOOTHAF_LOG_MEASURE, score, mean, sad,
                                laplacian, code);
}

static int smoothaf_measure_block_reason(void)
{
    int movie_dry_run = smoothaf_movie_dry_run_recording_allowed(
        smoothaf_vlog_af_mode, lv, is_movie_mode(), RECORDING_H264,
        RECORDING_CUSTOM, RECORDING_RAW);
    int movie_active = smoothaf_movie_active_recording_allowed(
        smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
        is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW);
    if (smoothaf_cancel) return SMOOTHAF_BLOCK_CANCELLED;
    if (!lv) return SMOOTHAF_BLOCK_NO_LV;
    if (movie_dry_run || movie_active)
    {
        if (is_manual_focus()) return SMOOTHAF_BLOCK_MANUAL_FOCUS;
        if (is_continuous_af()) return SMOOTHAF_BLOCK_CONTINUOUS_AF;
        if (is_follow_focus_active()) return SMOOTHAF_BLOCK_FOLLOW_FOCUS;
        if (is_rack_focus_enabled()) return SMOOTHAF_BLOCK_RACK_FOCUS;
        if (is_focus_stack_enabled()) return SMOOTHAF_BLOCK_FOCUS_STACK;
        return SMOOTHAF_BLOCK_NONE;
    }
    if (is_movie_mode()) return SMOOTHAF_BLOCK_MOVIE;
    if (RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW)
        return SMOOTHAF_BLOCK_RECORDING;
    return SMOOTHAF_BLOCK_NONE;
}

static void smoothaf_movie_recording_transition(int recording_class, uint32_t now);
static void smoothaf_movie_start_automatic(void);
static void smoothaf_movie_stop_automatic(void);

static unsigned int smoothaf_movie_recording_cbr(unsigned int ctx)
{
    int recording_class;
    (void)ctx;
    recording_class = smoothaf_movie_recording_class(RECORDING_H264,
                                                     RECORDING_CUSTOM,
                                                     RECORDING_RAW);
    smoothaf_movie_recording_transition(recording_class,
                                       (uint32_t)get_ms_clock());
    if (recording_class == SMOOTHAF_MOVIE_RECORDING_H264 &&
        smoothaf_movie_start_pending && !smoothaf_monitor.enabled &&
        !smoothaf_is_task_running())
        smoothaf_movie_start_automatic();
    return CBR_RET_CONTINUE;
}

static void smoothaf_movie_start_automatic(void)
{
    if (smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS ||
        !smoothaf_movie_active_experimental || !smoothaf_enabled ||
        smoothaf_panic || smoothaf_monitor.enabled)
        return;
    if (smoothaf_cancel)
    {
        smoothaf_movie_start_pending = 0;
        return;
    }
    if (smoothaf_is_task_running())
    {
        smoothaf_movie_start_pending = 1;
        return;
    }
    if (smoothaf_movie_start_pending == 0 && smoothaf_movie_recording.active)
        smoothaf_movie_start_pending = 1;
    if (!smoothaf_movie_active_gate_allowed(
            smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
            is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW,
            smoothaf_cancel, is_manual_focus(), is_continuous_af(),
            is_follow_focus_active(), is_rack_focus_enabled(),
            is_focus_stack_enabled()))
        return;
    smoothaf_monitor_start(&smoothaf_monitor, smoothaf_vlog_metric,
                           (uint32_t)get_ms_clock());
    smoothaf_vlog_algorithm_apply_at_episode_boundary();
    smoothaf_vlog_af_set_algorithm(&smoothaf_vlog_af,
                                   smoothaf_vlog_algorithm_active);
    smoothaf_monitor_task_requested = 1;
    smoothaf_monitor_deinit = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 0;
    smoothaf_cancel = 0;
    smoothaf_session_started_ms = (uint32_t)get_ms_clock();
    smoothaf_persist_logger_sync_config();
    smoothaf_persist_session_start();
    smoothaf_vlog_af_reset_session(&smoothaf_vlog_af, smoothaf_vlog_af_mode);
    smoothaf_vlog_af_arm_continuous(&smoothaf_vlog_af,
                                    smoothaf_vlog_direction_for_episode(),
                                    smoothaf_monitor.score);
    smoothaf_vlog_af_set_auto_probe(&smoothaf_vlog_af,
                                    smoothaf_vlog_direction_auto_selected());
    smoothaf_episode_begin(SMOOTHAF_EPISODE_EVENT_ARM);
    smoothaf_movie_automatic_session = 1;
    smoothaf_movie_start_pending = 0;
    smoothaf_vlog_af_set_episode_limit(&smoothaf_vlog_af,
                                       smoothaf_vlog_episode_limit);
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_ARMED;
    smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_ARMED;
    smoothaf_log_event_extended(SMOOTHAF_LOG_RECORDING, 4, 1, 0, 0, 0);
    if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))
    {
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_task_requested = 0;
        smoothaf_movie_automatic_session = 0;
        smoothaf_episode_stop();
        smoothaf_persist_session_end(4);
    }
}

static void smoothaf_movie_stop_automatic(void)
{
    smoothaf_movie_start_pending = 0;
    if (!smoothaf_movie_automatic_session || !smoothaf_monitor_task_requested ||
        smoothaf_movie_recording.active)
        return;
    smoothaf_request_episode_stop();
    smoothaf_task_stop_requested = 1;
    smoothaf_monitor_stop(&smoothaf_monitor);
    smoothaf_monitor_task_requested = 0;
    smoothaf_movie_automatic_session = 0;
    /* The monitor task owns final EPISODE_END/SESSION_END ordering. */
    if (!smoothaf_task_running)
        smoothaf_persist_session_end(2);
    smoothaf_log_event_extended(SMOOTHAF_LOG_RECORDING, 5, 0, 0, 0, 0);
}

static void smoothaf_movie_recording_transition(int recording_class, uint32_t now)
{
    int edge = smoothaf_movie_recording_observe(
        &smoothaf_movie_recording, recording_class, now);
    if (edge == 1)
    {
        smoothaf_log_event_extended(SMOOTHAF_LOG_RECORDING, 1,
                                    recording_class, 0, 0, 0);
        if (recording_class == SMOOTHAF_MOVIE_RECORDING_H264)
            smoothaf_movie_start_automatic();
        else if (smoothaf_movie_automatic_session)
            smoothaf_request_episode_stop();
    }
    else if (edge == 2)
    {
        smoothaf_log_event_extended(SMOOTHAF_LOG_RECORDING, 0,
                                    SMOOTHAF_MOVIE_RECORDING_NONE,
                                    (int)(now - smoothaf_movie_recording.started_ms),
                                    (int)smoothaf_movie_recording.samples,
                                    (int)smoothaf_movie_recording.valid);
        smoothaf_movie_stop_automatic();
    }
}

static int smoothaf_block_reason(void)
{
    int reason = smoothaf_measure_block_reason();
    if (reason) return reason;
    /* Only the explicit H.264 Movie Active experiment may reach lens_focus()
     * during Movie/recording. RAW and custom recording remain blocked. */
    if (is_movie_mode() && !smoothaf_movie_active_recording_allowed(
            smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
            is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW))
        return SMOOTHAF_BLOCK_MOVIE;
    if ((RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW) &&
        !smoothaf_movie_active_recording_allowed(
            smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
            is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW))
        return SMOOTHAF_BLOCK_RECORDING;
    if (is_manual_focus()) return SMOOTHAF_BLOCK_MANUAL_FOCUS;
    if (is_continuous_af()) return SMOOTHAF_BLOCK_CONTINUOUS_AF;
    if (is_follow_focus_active()) return SMOOTHAF_BLOCK_FOLLOW_FOCUS;
    if (is_rack_focus_enabled()) return SMOOTHAF_BLOCK_RACK_FOCUS;
    if (is_focus_stack_enabled()) return SMOOTHAF_BLOCK_FOCUS_STACK;
    return SMOOTHAF_BLOCK_NONE;
}

static int smoothaf_measure_with_metric(int metric, int *score_out,
                                        int *mean_out, int *sad_out,
                                        int *laplacian_out)
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
    if (!smoothaf_movie_dry_run_recording_allowed(
            smoothaf_vlog_af_mode, lv, is_movie_mode(), RECORDING_H264,
            RECORDING_CUSTOM, RECORDING_RAW) &&
        !smoothaf_movie_active_recording_allowed(
            smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
            is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW) &&
        (is_movie_mode() || RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW))
        return 0;
    vram = get_yuv422_vram();
    if (!vram || !vram->vram ||
        !smoothaf_geometry_ok(vram->width, vram->height, vram->pitch)) return 0;
    left = smoothaf_roi_left(vram->width);
    top = smoothaf_roi_top(vram->height);
    base = vram->vram + top * vram->pitch + left * 2;
    smoothaf_measure_all(base, vram->pitch, &sad, &laplacian, &mean);
    if (score_out) *score_out = metric == SMOOTHAF_METRIC_LAPLACIAN
                               ? laplacian : sad;
    if (mean_out) *mean_out = mean;
    if (sad_out) *sad_out = sad;
    if (laplacian_out) *laplacian_out = laplacian;
    return 1;
}

static int smoothaf_measure(int *score_out, int *mean_out, int *sad_out,
                            int *laplacian_out)
{
    return smoothaf_measure_with_metric(smoothaf_vlog_metric, score_out,
                                        mean_out, sad_out, laplacian_out);
}

static int smoothaf_measure_test(int *score_out, int *mean_out, int *sad_out,
                                 int *laplacian_out)
{
    return smoothaf_measure_with_metric(smoothaf_test_metric, score_out,
                                        mean_out, sad_out, laplacian_out);
}

static int smoothaf_move_with_step(int direction, int step)
{
    int start;
    int result;
    int reason;
    int movie_active;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    movie_active = smoothaf_movie_active_recording_allowed(
        smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
        is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW);
    if (!smoothaf_test_session &&
        (smoothaf_vlog_af.armed || smoothaf_vlog_af.continuous_armed) &&
        (smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_ACTIVE &&
         smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS))
        reason = SMOOTHAF_BLOCK_DISABLED;
    else if (!smoothaf_test_session &&
             (smoothaf_vlog_af.armed || smoothaf_vlog_af.continuous_armed) &&
             (smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE ||
              smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) &&
             !smoothaf_enabled)
        reason = SMOOTHAF_BLOCK_DISABLED;
    else if (direction != -1 && direction != 1) reason = SMOOTHAF_BLOCK_PARAMS;
    else if (!smoothaf_test_session && is_movie_mode() && movie_active &&
             !(smoothaf_vlog_af.armed || smoothaf_vlog_af.continuous_armed))
        reason = SMOOTHAF_BLOCK_DISABLED;
    else if (step < 1 || step > 3) reason = SMOOTHAF_BLOCK_PARAMS;
    else reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        if (!smoothaf_test_session)
            smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_GATE,
                                   SMOOTHAF_EPISODE_RESULT_BLOCKED,
                                   smoothaf_last_score, smoothaf_vlog_af.best,
                                   smoothaf_af_confidence, direction, step,
                                   smoothaf_vlog_af.current_position,
                                   (uint32_t)get_ms_clock(), (uint32_t)get_ms_clock());
        smoothaf_log_event_extended(SMOOTHAF_LOG_ABORT, reason, direction,
                                    step, 0, 0);
        smoothaf_telemetry_update(smoothaf_last_score, 0, direction,
                                  step, smoothaf_status, 1, reason);
        return 0;
    }
    if (smoothaf_af_move_attempts != INT32_MAX) smoothaf_af_move_attempts++;
    smoothaf_status = SMOOTHAF_STATUS_MOVING;
    smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_MOVING;
    start = get_ms_clock();
    smoothaf_last_lens_called = 0;
    smoothaf_log_event_extended(SMOOTHAF_LOG_MOVE, direction, step,
                                smoothaf_last_score, 0, 0);
    if (!smoothaf_test_session)
        smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_DECISION, 0,
                               smoothaf_last_score, smoothaf_vlog_af.best,
                               smoothaf_af_confidence, direction, step,
                               smoothaf_vlog_af.current_position,
                               (uint32_t)start, (uint32_t)get_ms_clock());
    smoothaf_telemetry_update(smoothaf_last_score, 0, direction, step,
                              smoothaf_status, 0, SMOOTHAF_BLOCK_NONE);
    if (!smoothaf_test_session &&
        (smoothaf_vlog_af.armed || smoothaf_vlog_af.continuous_armed) &&
        ((smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_ACTIVE &&
          smoothaf_vlog_af_mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) ||
         !smoothaf_enabled || smoothaf_panic || smoothaf_cancel ||
         smoothaf_task_stop_requested))
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_DISABLED;
        smoothaf_last_error = 1;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ABORT,
                                    smoothaf_last_block, direction,
                                    step, 0, 0);
        return 0;
    }
    smoothaf_last_lens_called = 1;
    if (smoothaf_af_lens_calls != INT32_MAX) smoothaf_af_lens_calls++;
    if (!smoothaf_test_session && smoothaf_vlog_af.lens_calls != UINT32_MAX)
        smoothaf_vlog_af.lens_calls++;
    if (!smoothaf_test_session)
        smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_LENS_CALL,
                               SMOOTHAF_EPISODE_RESULT_NONE,
                               smoothaf_last_score, smoothaf_vlog_af.best,
                               smoothaf_af_confidence, direction, step,
                               smoothaf_vlog_af.current_position,
                               (uint32_t)start, (uint32_t)start);
    smoothaf_vlog_af.last_lens_start_ms = (uint32_t)get_ms_clock();
    if (!smoothaf_test_session && smoothaf_vlog_af.armed)
        smoothaf_vlog_af_begin_deadline(&smoothaf_vlog_af,
                                        smoothaf_vlog_af.last_lens_start_ms);
    result = lens_focus(direction, step, 1, 0);
    smoothaf_vlog_af.last_lens_end_ms = (uint32_t)get_ms_clock();
    if (smoothaf_vlog_af.total_lens_ms <= UINT32_MAX - (smoothaf_vlog_af.last_lens_end_ms - smoothaf_vlog_af.last_lens_start_ms))
        smoothaf_vlog_af.total_lens_ms += smoothaf_vlog_af.last_lens_end_ms - smoothaf_vlog_af.last_lens_start_ms;
    smoothaf_last_lens_return = result;
    if (!smoothaf_test_session) smoothaf_af_episode_lens_return = result;
    if (!smoothaf_test_session)
        smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_LENS_RETURN,
                               result ? 0 : SMOOTHAF_EPISODE_RESULT_LENS_FAILED,
                               smoothaf_last_score, smoothaf_vlog_af.best,
                               smoothaf_af_confidence, direction, step,
                               smoothaf_vlog_af.current_position,
                               (uint32_t)start, (uint32_t)get_ms_clock());

    /* Persistent Logger: COMMAND hook (non-blocking producer) */
    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
    {
        uint32_t lens_ms = smoothaf_vlog_af.last_lens_end_ms - smoothaf_vlog_af.last_lens_start_ms;
        smoothaf_persist_log_controller(&smoothaf_persist_log,
                                        smoothaf_vlog_af.last_lens_end_ms,
                                        (uint16_t)smoothaf_af_episode_id,
                                        (uint8_t)smoothaf_vlog_af.state,
                                        (uint8_t)smoothaf_vlog_af.controller_action,
                                        (uint8_t)smoothaf_af_confidence,
                                        smoothaf_vlog_af.best);
        smoothaf_persist_log_command(&smoothaf_persist_log,
                             smoothaf_vlog_af.last_lens_end_ms,
                             (uint16_t)smoothaf_af_episode_id,
                             (uint8_t)smoothaf_vlog_af.commands,
                             (int8_t)direction, (uint8_t)step,
                             lens_ms, SMOOTHAF_SETTLE_MS, 0);
    }
    smoothaf_physical_movement_state = result ? SMOOTHAF_MOVEMENT_COMPLETE
                                               : SMOOTHAF_MOVEMENT_FAILED;
    /* lens_focus() returning nonzero means the Canon call was accepted.  It
     * does not confirm that the image score improved; that is measured later. */
    smoothaf_af_last_result_valid = result ? 1 : 0;
    smoothaf_log_event_extended(SMOOTHAF_LOG_CONTROLLER, direction,
                                step, smoothaf_last_focus_pos,
                                result, smoothaf_last_block);
    if (result && smoothaf_af_move_successes != INT32_MAX)
        smoothaf_af_move_successes++;
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - (uint32_t)start);
    smoothaf_last_focus_pos = lens_info.focus_pos;
    smoothaf_telemetry_update(smoothaf_last_score, smoothaf_last_time, direction,
                              step, smoothaf_status, result ? 0 : 1,
                              result ? 0 : SMOOTHAF_BLOCK_NONE);
    if (!result)
    {
        smoothaf_af_episode_lens_return = result;
        smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_FAILED;
        reason = smoothaf_block_reason();
        if (reason) smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR, smoothaf_last_block,
                                    result, smoothaf_last_lens_return,
                                    smoothaf_af_move_attempts,
                                    smoothaf_af_current_position);
        return 0;
    }
    msleep(SMOOTHAF_COOLDOWN_MS);
    if (!smoothaf_test_session)
        smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_SETTLE, 0,
                               smoothaf_last_score, smoothaf_vlog_af.best,
                               smoothaf_af_confidence, direction, step,
                               smoothaf_vlog_af.current_position,
                               (uint32_t)start, (uint32_t)get_ms_clock());
    return 1;
}

static int smoothaf_move(int direction)
{
    return smoothaf_move_with_step(direction, smoothaf_vlog_step);
}

/* The monitor owns one episode at a time.  Apply a changed algorithm only at
 * that boundary; a pending menu edit can never rewrite a controller in flight. */
static void smoothaf_vlog_algorithm_apply_at_episode_boundary(void)
{
    int selected = smoothaf_vlog_algorithm_value();
    if (smoothaf_vlog_algorithm_active == selected)
    {
        smoothaf_vlog_algorithm_pending_flag = 0;
        smoothaf_vlog_algorithm_pending = selected;
        return;
    }
    if (smoothaf_vlog_af.armed || smoothaf_vlog_af.awaiting_result) {
        smoothaf_vlog_algorithm_pending_flag = 1;
        smoothaf_vlog_algorithm_pending = selected;
        return;
    }
    smoothaf_vlog_algorithm_active = selected;
    smoothaf_vlog_algorithm_episode = selected;
    smoothaf_vlog_algorithm_pending_flag = 0;
    smoothaf_vlog_algorithm_pending = selected;
}

static void smoothaf_vlog_algorithm_start_episode(void)
{
    smoothaf_vlog_algorithm_apply_at_episode_boundary();
    smoothaf_vlog_algorithm_episode = smoothaf_vlog_algorithm_active;
    smoothaf_vlog_af_set_algorithm(&smoothaf_vlog_af,
                                   smoothaf_vlog_algorithm_active);
}

static int smoothaf_vlog_controller_next(int *direction, int *step)
{
    /* Shared seam calls smoothaf_vlog_af_auto_needs_move() before the selected
     * controller, preserving the production AUTO ordering. */
    return smoothaf_episode_next_command(
        &smoothaf_vlog_af, smoothaf_vlog_direction,
        smoothaf_vlog_probe_step(), smoothaf_vlog_step,
        direction, step);
}

static int smoothaf_vlog_controller_on_score(int score)
{
    return smoothaf_episode_on_score(&smoothaf_vlog_af, score);
}

static void smoothaf_vlog_adapter_prepare_score(int score, uint32_t now)
{
    smoothaf_episode_prepare_score(&smoothaf_vlog_af, score, now);
}

static void smoothaf_vlog_controller_rollback_move(int direction, int step)
{
    smoothaf_episode_rollback_move(&smoothaf_vlog_af, direction, step);
}

static void smoothaf_vlog_controller_rollback_recovery(void)
{
    smoothaf_episode_rollback_recovery(&smoothaf_vlog_af);
}

static int smoothaf_search_move(void *ctx, int dir)
{
    (void)ctx;
    return smoothaf_move_with_step(dir, smoothaf_test_step);
}

static int smoothaf_search_measure(void *ctx, int *score)
{
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    (void)ctx;
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    msleep(SMOOTHAF_SETTLE_MS);
    if (!smoothaf_measure_test(score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        return 0;
    }
    smoothaf_publish_measurement(*score, mean, sad, laplacian, 1,
                                 smoothaf_has_confidence(*score, mean)
                                 ? SMOOTHAF_MEASURE_OK : SMOOTHAF_MEASURE_LOW);
    return 1;
}

static int smoothaf_search_abort(void *ctx)
{
    uint32_t start = *(uint32_t *)ctx;
    int reason;
    if (smoothaf_cancel) return SMOOTHAF_RESULT_CANCELLED;
    if ((uint32_t)get_ms_clock() - start > SMOOTHAF_TIMEOUT_MS)
        return SMOOTHAF_RESULT_TIMEOUT;
    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        return (reason == SMOOTHAF_BLOCK_CANCELLED ||
                reason == SMOOTHAF_BLOCK_DISABLED)
             ? SMOOTHAF_RESULT_CANCELLED : SMOOTHAF_RESULT_ERROR;
    }
    return 0;
}

static int smoothaf_auto_move(void *ctx, int direction)
{
    (void)ctx;
    return smoothaf_move_with_step(direction, smoothaf_test_step);
}

static int smoothaf_auto_measure(void *ctx, int *score)
{
    (void)ctx;
    if (!smoothaf_search_measure(ctx, score))
        return SMOOTHAF_AUTO_MEASURE_INVALID;
    if (!smoothaf_last_measure_valid)
        return SMOOTHAF_AUTO_MEASURE_INVALID;
    if (!smoothaf_has_confidence(*score, smoothaf_last_mean))
        return SMOOTHAF_AUTO_MEASURE_LOW;
    return SMOOTHAF_AUTO_MEASURE_VALID;
}

static int smoothaf_auto_abort(void *ctx)
{
    return smoothaf_search_abort(ctx);
}

static int smoothaf_status_from_result(enum smoothaf_result result)
{
    switch (result)
    {
        case SMOOTHAF_RESULT_LOCKED: return SMOOTHAF_STATUS_LOCKED;
        case SMOOTHAF_RESULT_LIMIT: return SMOOTHAF_STATUS_SEARCH_LIMIT;
        case SMOOTHAF_RESULT_NO_CONFIDENCE: return SMOOTHAF_STATUS_NO_CONFIDENCE;
        case SMOOTHAF_RESULT_CANCELLED: return SMOOTHAF_STATUS_CANCELLED;
        case SMOOTHAF_RESULT_TIMEOUT: return SMOOTHAF_STATUS_TIMEOUT;
        case SMOOTHAF_RESULT_LENS_ERROR: return SMOOTHAF_STATUS_LENS_ERROR;
        default: return SMOOTHAF_STATUS_ERROR;
    }
}

#define SMOOTHAF_VLOG_AF_ERROR_LOG_BASE 100

static void smoothaf_vlog_af_fail(int result, int block)
{
    if (smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_IDLE &&
        smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_COMPLETE &&
        smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_ABORTED)
    {
        smoothaf_recovery_cancel(&smoothaf_vlog_af.recovery);
        smoothaf_vlog_af_reserve_recovery(&smoothaf_vlog_af, 0);
    }
    int telemetry_result = smoothaf_episode_result_map(result);
    int continuous = smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                     smoothaf_vlog_af.continuous_armed;
    uint32_t now = (uint32_t)get_ms_clock();
    smoothaf_af_episode_state_before_error = smoothaf_vlog_af.state;
    smoothaf_af_episode_reason = result;
    smoothaf_af_episode_result = result;
    if (result == SMOOTHAF_VLOG_AF_RESULT_FAILED)
        smoothaf_af_episode_lens_return = 0;
    if (block != SMOOTHAF_VLOG_AF_BLOCK_NONE)
    {
        smoothaf_af_command_block = block;
        smoothaf_af_episode_block_reason = block;
    }
    if (continuous)
    {
        if (smoothaf_vlog_af.acquisition_attempt_active)
            smoothaf_vlog_af_note_acquisition_failure(&smoothaf_vlog_af, now);
        if (result == SMOOTHAF_VLOG_AF_RESULT_CANCELLED)
            smoothaf_af_episode_cancel_reason = result;
        smoothaf_vlog_af_pause_continuous(&smoothaf_vlog_af, result, now);
    }
    else
    {
        smoothaf_vlog_af_stop(&smoothaf_vlog_af);
        smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_ERROR;
    }
    smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
    smoothaf_episode_summary_publish(telemetry_result, now);
    smoothaf_log_event_extended(SMOOTHAF_LOG_ERROR,
                                SMOOTHAF_VLOG_AF_ERROR_LOG_BASE + result,
                                smoothaf_vlog_af.state,
                                (int)smoothaf_vlog_af.commands,
                                (int)smoothaf_vlog_af_commands_available(&smoothaf_vlog_af),
                                block);

    /* Persistent Logger: TIMEOUT/CANCELLED/LIMIT hooks (non-blocking producer) */
    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
    {
        if (result == SMOOTHAF_VLOG_AF_RESULT_TIMEOUT)
        {
            /* Determine phase at timeout */
            uint8_t phase = 0;
            if (smoothaf_vlog_af.auto_probe_enabled && !smoothaf_vlog_af.auto_probe_done)
                phase = 1; /* AUTO probe phase */
            else if (smoothaf_vlog_af.state == SMOOTHAF_VLOG_AF_SETTLING)
                phase = 2; /* Settling phase */
            else if (smoothaf_vlog_af.state == SMOOTHAF_VLOG_AF_PROBING)
                phase = 3; /* Algorithm phase */
            else
                phase = 4; /* Other/cooldown */
            smoothaf_persist_log_timeout(&smoothaf_persist_log, now,
                                 (uint16_t)smoothaf_af_episode_id,
                                 phase, now - smoothaf_vlog_af.started_ms,
                                 (uint8_t)smoothaf_vlog_af.commands);
        }
        else if (result == SMOOTHAF_VLOG_AF_RESULT_CANCELLED)
        {
            uint8_t cancel_reason = (smoothaf_panic ? 2 :
                                     (smoothaf_task_stop_requested ? 1 :
                                      (block == SMOOTHAF_VLOG_AF_BLOCK_DISABLED ? 3 : 4)));
            smoothaf_persist_log_cancelled(&smoothaf_persist_log, now,
                                           (uint16_t)smoothaf_af_episode_id,
                                           cancel_reason,
                                           (uint8_t)smoothaf_vlog_af.commands);

        }
        else if (result == SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT ||
                 result == SMOOTHAF_VLOG_AF_RESULT_BLOCKED)
        {
            uint8_t limit_type = result == SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT
                               ? SMOOTHAF_PERSIST_LOG_LIMIT_EPISODE_CMDS
                               : SMOOTHAF_PERSIST_LOG_LIMIT_SESSION_CMDS;
            smoothaf_persist_log_limit(&smoothaf_persist_log, now,
                               (uint16_t)smoothaf_af_episode_id,
                               limit_type,
                               (uint32_t)smoothaf_vlog_af.commands);
        }
    }
}

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
    int gate_reason;
    int command_direction;
    int command_step;
    int adapter_continue;
    int recovery_direction;
    int recovery_step;
    int recovery_active;
    int auto_probe_measurement;
    int post_move_sample;
    int post_move_lighting;
    int monitor_accept_done;
    int acquire_continuous;
    struct smoothaf_monitor_sample sample;
    while (smoothaf_monitor.enabled && !smoothaf_cancel &&
           !smoothaf_monitor_deinit && !smoothaf_task_stop_requested)
    {
        /* Observe recording edges for telemetry.  Authorization is checked
         * independently immediately before every physical lens command. */
        smoothaf_movie_recording_transition(
            smoothaf_movie_recording_class(RECORDING_H264, RECORDING_CUSTOM,
                                           RECORDING_RAW),
            (uint32_t)get_ms_clock());
        if (smoothaf_measure_requested && !smoothaf_vlog_af.awaiting_result)
        {
            smoothaf_measure_requested = 0;
            smoothaf_measure_task();
        }
        /* A stale episode must not issue another move after its time budget. */
        if (smoothaf_vlog_af_timeout_due(&smoothaf_vlog_af,
                                         (uint32_t)get_ms_clock()))
            smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_TIMEOUT,
                                  SMOOTHAF_VLOG_AF_BLOCK_NONE);
        if (smoothaf_episode_cancel_requested())
        {
            smoothaf_episode_cancel();
            smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
        }
        smoothaf_vlog_af_update_continuous_latch(
            &smoothaf_vlog_af, smoothaf_monitor.state,
            (uint32_t)get_ms_clock());
        recovery_active = smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_IDLE &&
                          smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_COMPLETE &&
                          smoothaf_vlog_af.recovery.phase != SMOOTHAF_RECOVERY_ABORTED;
        if (!smoothaf_monitor_due((uint32_t)get_ms_clock(),
                                  smoothaf_monitor.metrics.next_due_ms))
        {
            msleep(20);
            continue;
        }
        if (smoothaf_measure_block_reason())
        {
            smoothaf_last_block = smoothaf_measure_block_reason();
            if (smoothaf_vlog_af.armed && smoothaf_vlog_af.awaiting_result)
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_BLOCKED,
                                      SMOOTHAF_VLOG_AF_BLOCK_WAITING_RESULT);
            smoothaf_monitor_record_blocked(&smoothaf_monitor);
            smoothaf_monitor_record_skip(&smoothaf_monitor);
            smoothaf_monitor_schedule_after(&smoothaf_monitor, (uint32_t)get_ms_clock());
            smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_PAUSED);
            smoothaf_monitor_log_transitions();
            msleep(20);
            continue;
        }
        start = (uint32_t)get_ms_clock();
        smoothaf_monitor_record_start(&smoothaf_monitor, start);
        if (smoothaf_monitor.metrics.attempts != UINT32_MAX) smoothaf_monitor.metrics.attempts++;
        valid = smoothaf_measure(&score, &mean, &sad, &laplacian);
        completed = (uint32_t)get_ms_clock();
        sample.score = score;
        sample.mean = mean;
        sample.sad = sad;
        sample.laplacian = laplacian;
        sample.valid = valid;
        sample.confident = valid && smoothaf_has_confidence(score, mean);
        sample.metric = smoothaf_vlog_metric == SMOOTHAF_METRIC_LAPLACIAN
                      ? SMOOTHAF_MONITOR_METRIC_LAPLACIAN : SMOOTHAF_MONITOR_METRIC_SAD;
        sample.duration_ms = completed - start;
        /* Instrumentation: capture sample time and accumulate settle/wait durations */
        smoothaf_vlog_af.last_sample_ms = completed;

        /* Persistent Logger: MEASUREMENT hook (non-blocking producer) */
        if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log) && smoothaf_vlog_af.armed)
        {
            smoothaf_persist_log_measurement(&smoothaf_persist_log, completed,
                                     (uint16_t)smoothaf_af_episode_id,
                                     score, mean, sad, laplacian,
                                     (uint8_t)(valid ? 1 : 0),
                                     (uint8_t)(sample.confident ? 1 : 0));
        }
        if (smoothaf_vlog_af.last_settle_start_ms &&
            completed >= smoothaf_vlog_af.last_settle_start_ms &&
            smoothaf_vlog_af.total_settle_ms <= UINT32_MAX - (completed - smoothaf_vlog_af.last_settle_start_ms))
        {
            smoothaf_vlog_af.total_settle_ms += completed - smoothaf_vlog_af.last_settle_start_ms;
            smoothaf_vlog_af.last_settle_start_ms = 0;
        }
        if (smoothaf_vlog_af.last_lens_end_ms &&
            completed >= smoothaf_vlog_af.last_lens_end_ms + SMOOTHAF_COOLDOWN_MS &&
            smoothaf_vlog_af.total_sample_wait_ms <= UINT32_MAX - (completed - smoothaf_vlog_af.last_lens_end_ms - SMOOTHAF_COOLDOWN_MS))
        {
            smoothaf_vlog_af.total_sample_wait_ms += completed - smoothaf_vlog_af.last_lens_end_ms - SMOOTHAF_COOLDOWN_MS;
            smoothaf_vlog_af.last_lens_end_ms = 0;
        }
        if (smoothaf_movie_recording.active &&
            smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_DRY_RUN)
        {
            if (smoothaf_movie_recording.samples != UINT32_MAX)
                smoothaf_movie_recording.samples++;
            if (sample.valid && sample.confident)
            {
                if (smoothaf_movie_recording.valid != UINT32_MAX)
                    smoothaf_movie_recording.valid++;
            }
            else if (smoothaf_movie_recording.invalid != UINT32_MAX)
                smoothaf_movie_recording.invalid++;
            smoothaf_log_event_extended(SMOOTHAF_LOG_RECORDING,
                                        sample.valid ? 2 : 3,
                                        sample.score, (int)sample.duration_ms,
                                        sample.confident, smoothaf_monitor.state);
        }
        if (sample.duration_ms > smoothaf_monitor_interval_ms(smoothaf_monitor.interval_index) &&
            smoothaf_monitor.metrics.overruns != UINT32_MAX) smoothaf_monitor.metrics.overruns++;
        post_move_sample = smoothaf_vlog_af.awaiting_result;
        post_move_lighting = 0;
        monitor_accept_done = 0;
        if (!post_move_sample)
            smoothaf_monitor_accept(&smoothaf_monitor, &sample, completed);
        else
        {
            /* Observe the exposure independently of the STM command result.
             * The sample is incorporated into the environmental reference only
             * after the controller has classified its own result. */
            post_move_lighting = sample.valid &&
                smoothaf_monitor_lighting_changed(&smoothaf_monitor, sample.mean);
            smoothaf_monitor_schedule_after(&smoothaf_monitor, completed);
        }
        smoothaf_af_confidence = sample.confident;
        if (smoothaf_vlog_af.awaiting_result && smoothaf_vlog_af.started_ms)
        {
            if (smoothaf_vlog_af.image_ms <= UINT32_MAX - sample.duration_ms)
                smoothaf_vlog_af.image_ms += sample.duration_ms;
        }
        smoothaf_telemetry_update(valid ? score : -1, (int)sample.duration_ms, 0,
                                  smoothaf_vlog_af.command_step,
                                  smoothaf_monitor.state, valid ? 0 : 1,
                                  valid ? 0 : SMOOTHAF_BLOCK_FRAME);
        result_processed = 0;
        auto_probe_measurement = smoothaf_vlog_af.armed &&
                                 !smoothaf_vlog_af.auto_probe_done &&
                                 smoothaf_vlog_direction_auto_selected() &&
                                 smoothaf_vlog_af_result_due(&smoothaf_vlog_af, completed);
        if (smoothaf_vlog_af.armed && smoothaf_vlog_af_result_due(&smoothaf_vlog_af, completed))
        {
            if (smoothaf_vlog_af_timeout_due(&smoothaf_vlog_af, completed))
            {
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_TIMEOUT,
                                      SMOOTHAF_VLOG_AF_BLOCK_NONE);
                smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_FAILED;
            }
            else if (post_move_lighting)
            {
                if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                    smoothaf_persist_log_environmental(&smoothaf_persist_log, completed,
                                                       (uint16_t)smoothaf_af_episode_id,
                                                       1, sample.mean,
                                                       sample.mean - smoothaf_monitor.reference_mean);
                /* The exposure changed between the command and its result.
                 * Do not credit or blame the STM command, and do not add an AF
                 * failure to the safety streak. */
                smoothaf_vlog_af.post_move_score = sample.score;
                smoothaf_vlog_af.score_delta =
                    sample.score - smoothaf_vlog_af.previous_score;
                smoothaf_vlog_af.last_result_ms = completed;
                smoothaf_vlog_af.uncertain = 1;
                smoothaf_af_post_move_valid = 0;
                smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
                smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
                smoothaf_af_episode_block_reason = SMOOTHAF_VLOG_AF_BLOCK_NONE;
                smoothaf_af_post_move_score = smoothaf_vlog_af.post_move_score;
                smoothaf_af_best_score = smoothaf_vlog_af.best;
                smoothaf_af_score_delta = smoothaf_vlog_af.score_delta;
                smoothaf_af_best_position = smoothaf_vlog_af.best_position;
                smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                smoothaf_af_last_direction = smoothaf_vlog_af.last_direction;
                smoothaf_af_decision_relation = 0;
                smoothaf_vlog_af_abort_environmental(&smoothaf_vlog_af, completed);
                smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_COMPLETE;
                smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_POST_MOVE,
                    SMOOTHAF_EPISODE_RESULT_UNCERTAIN, sample.score,
                    smoothaf_vlog_af.best, sample.confident,
                    smoothaf_vlog_af.last_direction, smoothaf_vlog_af.command_step,
                    smoothaf_vlog_af.current_position,
                    smoothaf_vlog_af.last_command_ms, completed);
                smoothaf_episode_summary_publish(
                    smoothaf_episode_result_map(SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN),
                    completed);
                result_processed = 1;
            }
            else if (!sample.valid || !sample.confident || smoothaf_monitor.state == SMOOTHAF_MONITOR_UNCERTAIN)
            {
                smoothaf_vlog_af.uncertain = 1;
                smoothaf_af_post_move_valid = 0;
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
                                      SMOOTHAF_VLOG_AF_BLOCK_NONE);
                smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_FAILED;
            }
            else
            {
                smoothaf_af_confidence = sample.confident;
                if (auto_probe_measurement)
                {
                    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                        smoothaf_persist_log_auto_probe(&smoothaf_persist_log, completed,
                                                        (uint16_t)smoothaf_af_episode_id,
                                                        1, (int8_t)smoothaf_vlog_af.direction,
                                                        sample.score,
                                                        sample.score - smoothaf_vlog_af.previous_score,
                                                        (uint8_t)sample.confident);
                    smoothaf_vlog_adapter_prepare_score(sample.score, completed);
                    smoothaf_vlog_af_auto_observe_mean(&smoothaf_vlog_af, sample.mean);
                    adapter_continue = smoothaf_vlog_af_auto_on_score(
                        &smoothaf_vlog_af, sample.score);
                    (void)adapter_continue;
                    smoothaf_vlog_af.episode_result =
                        SMOOTHAF_VLOG_AF_RESULT_IMPROVED;
                    smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_PROBING;
                    adapter_continue = 1;
                }
                else if (smoothaf_vlog_af.algorithm == SMOOTHAF_ALGORITHM_CURRENT_VLOG &&
                         !recovery_active && !smoothaf_vlog_af.controller_measure_only)
                    smoothaf_vlog_af_record_result(&smoothaf_vlog_af, sample.score, completed);
                else
                    smoothaf_vlog_adapter_prepare_score(sample.score, completed);
                if (recovery_active)
                {
                    adapter_continue = smoothaf_recovery_on_score(
                        &smoothaf_vlog_af.recovery, sample.score);
                    smoothaf_vlog_af.post_move_score = sample.score;
                    smoothaf_vlog_af.last_result_ms = completed;
                    smoothaf_vlog_af.best = smoothaf_vlog_af.recovery.best_score;
                    smoothaf_vlog_af.best_position =
                        smoothaf_vlog_af.recovery.best_position;
                    smoothaf_vlog_af.current_position =
                        smoothaf_vlog_af.recovery.position;
                    if (!adapter_continue &&
                        (smoothaf_vlog_af.recovery.phase == SMOOTHAF_RECOVERY_COMPLETE ||
                         smoothaf_vlog_af.recovery.phase == SMOOTHAF_RECOVERY_ABORTED))
                    {
                        recovery_active = 0;
                        smoothaf_vlog_af_reserve_recovery(&smoothaf_vlog_af, 0);
                        smoothaf_vlog_af.episode_result =
                                    smoothaf_vlog_af.recovery.result ==
                            SMOOTHAF_RECOVERY_BEST_RECOVERED
                          ? SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED
                          : smoothaf_vlog_af.recovery.result ==
                            SMOOTHAF_RECOVERY_CANCELLED
                          ? SMOOTHAF_VLOG_AF_RESULT_CANCELLED
                          : smoothaf_vlog_af.recovery.result ==
                            SMOOTHAF_RECOVERY_LIMIT
                          ? SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT
                          : SMOOTHAF_VLOG_AF_RESULT_BEST_NOT_RECOVERED;
                        if (smoothaf_vlog_af.recovery.result ==
                            SMOOTHAF_RECOVERY_BEST_RECOVERED)
                            smoothaf_vlog_af_note_confirmed_success(
                                &smoothaf_vlog_af);
                        else
                            smoothaf_vlog_af_note_neutral_limit(
                                &smoothaf_vlog_af);
                        if (smoothaf_vlog_af.mode ==
                            SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                            smoothaf_vlog_af.continuous_armed)
                            smoothaf_vlog_af_complete_continuous_episode(
                                &smoothaf_vlog_af, completed);
                        else
                        {
                            smoothaf_vlog_af.armed = 0;
                            smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_COMPLETE;
                        }
                    }
                }
                if (!auto_probe_measurement && smoothaf_vlog_af.controller_measure_only)
                {
                    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                        smoothaf_persist_log_controller(&smoothaf_persist_log, completed,
                                                        (uint16_t)smoothaf_af_episode_id,
                                                        (uint8_t)smoothaf_vlog_af.state,
                                                        (uint8_t)smoothaf_vlog_af.controller_action,
                                                        (uint8_t)sample.confident,
                                                        smoothaf_vlog_af.best);
                    smoothaf_vlog_af.controller_measure_only = 0;
                    adapter_continue = smoothaf_vlog_controller_on_score(sample.score);
                    if (adapter_continue == 2)
                    {
                        smoothaf_vlog_af.controller_measure_only = 1;
                        smoothaf_vlog_af.awaiting_result = 1;
                        smoothaf_vlog_af.settle_until_ms = completed;
                        smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_SETTLING;
                    }
                    else if (!adapter_continue)
                    {
                        int controller_result = smoothaf_vlog_af_controller_result(
                            smoothaf_vlog_af.controller_action, sample.confident, 1);
                        int controller_locked = controller_result ==
                            SMOOTHAF_VLOG_AF_RESULT_CONVERGED;
                        smoothaf_vlog_af.episode_result = controller_result;
                        if (controller_locked)
                        {
                            smoothaf_vlog_af_note_confirmed_success(&smoothaf_vlog_af);
                            if (smoothaf_vlog_af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                                smoothaf_vlog_af.continuous_armed)
                                smoothaf_vlog_af_complete_continuous_episode(
                                    &smoothaf_vlog_af, completed);
                            else
                            {
                                smoothaf_vlog_af.armed = 0;
                                smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_COMPLETE;
                            }
                        }
                        else
                        {
                            smoothaf_vlog_af_note_neutral_limit(&smoothaf_vlog_af);
                            if (smoothaf_vlog_af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                                smoothaf_vlog_af.continuous_armed)
                                smoothaf_vlog_af_complete_continuous_episode(
                                    &smoothaf_vlog_af, completed);
                            else
                            {
                                smoothaf_vlog_af.armed = 0;
                                smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_COMPLETE;
                            }
                        }
                    }
                }
                else if (!auto_probe_measurement &&
                         smoothaf_vlog_af.algorithm != SMOOTHAF_ALGORITHM_CURRENT_VLOG &&
                         !recovery_active)
                {
                    if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                        smoothaf_persist_log_controller(&smoothaf_persist_log, completed,
                                                        (uint16_t)smoothaf_af_episode_id,
                                                        (uint8_t)smoothaf_vlog_af.state,
                                                        (uint8_t)smoothaf_vlog_af.controller_action,
                                                        (uint8_t)sample.confident,
                                                        smoothaf_vlog_af.best);
                    adapter_continue = smoothaf_vlog_controller_on_score(sample.score);
                    if (smoothaf_vlog_af.controller_measure_only)
                    {
                        smoothaf_vlog_af.awaiting_result = 1;
                        smoothaf_vlog_af.settle_until_ms = completed;
                    }
                    if (!adapter_continue && smoothaf_vlog_af.armed &&
                        !smoothaf_vlog_af.controller_measure_only &&
                        smoothaf_vlog_af.controller_action != SMOOTHAF_ADAPTIVE_CONFIRM &&
                        smoothaf_vlog_af.controller_action != SMOOTHAF_NORMAL_CONFIRM)
                    {
                        smoothaf_vlog_af.armed = 0;
                        smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_COMPLETE;
                        int controller_result = smoothaf_vlog_af_controller_result(
                            smoothaf_vlog_af.controller_action, sample.confident, 1);
                        int controller_locked = controller_result ==
                            SMOOTHAF_VLOG_AF_RESULT_CONVERGED;
                        smoothaf_vlog_af.episode_result = controller_result;
                        if (controller_locked)
                        {
                            smoothaf_vlog_af_note_confirmed_success(&smoothaf_vlog_af);
                            if (smoothaf_vlog_af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                                smoothaf_vlog_af.continuous_armed)
                                smoothaf_vlog_af_complete_continuous_episode(
                                    &smoothaf_vlog_af, completed);
                        }
                        else if (smoothaf_vlog_af.controller_action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE ||
                                 smoothaf_vlog_af.controller_action == SMOOTHAF_NORMAL_NO_CONFIDENCE ||
                                 smoothaf_vlog_af.controller_action == SMOOTHAF_HC_NO_CONFIDENCE)
                            smoothaf_vlog_af.episode_result = SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
                        else
                            smoothaf_vlog_af.episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
                        if (!controller_locked)
                            smoothaf_vlog_af_note_neutral_limit(&smoothaf_vlog_af);
                        if (smoothaf_vlog_af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
                            smoothaf_vlog_af.continuous_armed)
                            smoothaf_vlog_af_complete_continuous_episode(
                                &smoothaf_vlog_af, completed);
                    }
                }
                if (smoothaf_vlog_af.episode_result == SMOOTHAF_VLOG_AF_RESULT_WORSE &&
                    smoothaf_vlog_af.direction != smoothaf_vlog_af.last_direction)
                    smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_REVERSAL,
                                           SMOOTHAF_EPISODE_RESULT_WORSE,
                                           sample.score, smoothaf_vlog_af.best,
                                           sample.confident, smoothaf_vlog_af.direction,
                                           smoothaf_vlog_af.command_step, smoothaf_vlog_af.current_position,
                                           smoothaf_vlog_af.last_command_ms, completed);
                smoothaf_vlog_af.movement_ms = smoothaf_vlog_af.last_result_ms >=
                                               smoothaf_vlog_af.last_command_ms
                                             ? smoothaf_vlog_af.last_result_ms -
                                               smoothaf_vlog_af.last_command_ms : 0;
                smoothaf_af_decision_relation = smoothaf_vlog_af.score_delta > 0 ? 1 :
                                               smoothaf_vlog_af.score_delta < 0 ? -1 : 0;
                smoothaf_episode_event(SMOOTHAF_EPISODE_EVENT_POST_MOVE,
                                       smoothaf_episode_result_map(smoothaf_vlog_af.episode_result),
                                       sample.score, smoothaf_vlog_af.best,
                                       sample.confident,
                                       smoothaf_vlog_af.last_direction,
                                       smoothaf_vlog_af.command_step,
                                       smoothaf_vlog_af.current_position,
                                       smoothaf_vlog_af.last_command_ms,
                                       completed);
                smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_COMPLETE;
                smoothaf_af_post_move_valid = 1;
                smoothaf_af_episode_reason = smoothaf_vlog_af.episode_result;
                smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
                smoothaf_af_post_move_score = smoothaf_vlog_af.post_move_score;
                smoothaf_af_best_score = smoothaf_vlog_af.best;
                smoothaf_af_score_delta = smoothaf_vlog_af.score_delta;
                smoothaf_af_best_position = smoothaf_vlog_af.best_position;
                smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                smoothaf_af_last_direction = smoothaf_vlog_af.last_direction;
                if (!smoothaf_vlog_af.armed &&
                    smoothaf_vlog_af.episode_result != SMOOTHAF_VLOG_AF_RESULT_NONE)
                    smoothaf_episode_summary_publish(
                        smoothaf_episode_result_map(smoothaf_vlog_af.episode_result),
                        completed);
                result_processed = 1;
            }
        }
        if (post_move_sample && !monitor_accept_done)
        {
            /* Incorporate the deferred post-move sample exactly once, after
             * the controller has finished interpreting its own result. */
            smoothaf_monitor_accept(&smoothaf_monitor, &sample, completed);
            monitor_accept_done = 1;
            smoothaf_monitor_log_transitions();
        }
        if (result_processed && !post_move_lighting &&
            smoothaf_monitor.reference_state != SMOOTHAF_MONITOR_REF_VALID &&
            !smoothaf_vlog_af.armed &&
            smoothaf_vlog_af_result_qualifies_reference(
                smoothaf_vlog_af.episode_result,
                smoothaf_vlog_af.auto_probe_done,
                smoothaf_vlog_af.auto_probe_result,
                (int)smoothaf_vlog_af.algorithm_commands,
                smoothaf_vlog_af.best,
                smoothaf_vlog_af.initial_score,
                sample.score,
                sample.valid && sample.confident,
                !smoothaf_vlog_direction_auto_selected() ||
                smoothaf_vlog_af.auto_probe_lighting_valid) &&
            sample.valid && sample.confident)
        {
            smoothaf_monitor_qualify_reference(&smoothaf_monitor, sample.score,
                                               sample.mean, sample.metric,
                                               completed);
            if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                smoothaf_persist_log_reference_set(&smoothaf_persist_log, completed,
                                                   (uint16_t)smoothaf_af_episode_id,
                                                   (uint8_t)smoothaf_monitor.reference_state,
                                                   sample.score,
                                                   smoothaf_vlog_af.current_position);
            smoothaf_vlog_af.acquisition_attempt_active = 0;
            smoothaf_monitor_log_transitions();
        }
        else if (result_processed && !post_move_lighting &&
                 !smoothaf_vlog_af.armed &&
                 smoothaf_vlog_af.acquisition_attempt_active)
            smoothaf_vlog_af_note_acquisition_failure(
                &smoothaf_vlog_af, completed);
        acquire_continuous = sample.valid && sample.confident &&
            smoothaf_monitor.state == SMOOTHAF_MONITOR_STABLE &&
            smoothaf_vlog_af_should_acquire_continuous(
                &smoothaf_vlog_af, smoothaf_monitor.reference_state,
                smoothaf_monitor.reference_epoch, smoothaf_vlog_af_mode,
                smoothaf_enabled, completed);
        if (!smoothaf_measure_block_reason() && !smoothaf_block_reason() &&
            (smoothaf_vlog_af_should_resume_continuous(
                &smoothaf_vlog_af, smoothaf_monitor.state,
                smoothaf_vlog_af_mode, smoothaf_enabled, completed) ||
             acquire_continuous))
        {
            smoothaf_vlog_algorithm_start_episode();
            smoothaf_vlog_af_start_continuous_episode(
                &smoothaf_vlog_af, smoothaf_vlog_direction_for_episode(),
                sample.score);
            if (acquire_continuous)
                smoothaf_vlog_af_begin_acquisition(
                    &smoothaf_vlog_af, smoothaf_monitor.reference_epoch);
            if (smoothaf_vlog_direction_auto_selected())
                smoothaf_vlog_af_auto_begin_measurement(
                    &smoothaf_vlog_af, sample.mean);
            smoothaf_vlog_af.image_ms = 0;
            smoothaf_vlog_af.wait_ms = 0;
            smoothaf_vlog_af.movement_ms = 0;
            smoothaf_vlog_af.stabilization_ms = 0;
            smoothaf_vlog_af_set_episode_limit(&smoothaf_vlog_af,
                                               smoothaf_vlog_episode_limit);
            smoothaf_episode_begin(acquire_continuous
                                   ? SMOOTHAF_EPISODE_EVENT_ACQUIRE
                                   : SMOOTHAF_EPISODE_EVENT_BLUR);
            smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_ARMED;
            smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_ARMED;
            smoothaf_af_commands_episode = 0;
            smoothaf_af_initial_score = sample.score;
            smoothaf_af_post_move_valid = 0;
            smoothaf_af_last_result_valid = 0;
        }
        if (sample.valid && sample.confident &&
            smoothaf_vlog_af_should_probe_reference(
                &smoothaf_vlog_af, smoothaf_monitor.state,
                smoothaf_monitor.reference_state, smoothaf_vlog_af_mode))
        {
            smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_PROBING;
            if (smoothaf_monitor.reference_state ==
                    SMOOTHAF_MONITOR_REF_PROVISIONAL &&
                smoothaf_monitor.state != SMOOTHAF_MONITOR_POSSIBLE_BLUR)
                smoothaf_vlog_af.acquisition_epoch =
                    smoothaf_monitor.reference_epoch;
        }
        if (!recovery_active && smoothaf_vlog_af.armed &&
            smoothaf_vlog_af.mode != SMOOTHAF_VLOG_AF_DRY_RUN &&
            smoothaf_vlog_af.algorithm == SMOOTHAF_ALGORITHM_CURRENT_VLOG &&
            smoothaf_vlog_af.recovery.phase == SMOOTHAF_RECOVERY_IDLE &&
            sample.valid && sample.confident &&
            (!smoothaf_vlog_direction_auto_selected() ||
             smoothaf_vlog_af.auto_probe_done) &&
            smoothaf_far_focus_recovery &&
            smoothaf_recovery_should_start(smoothaf_enabled, sample.score,
                                           smoothaf_movie_recording.active,
                                           smoothaf_movie_active_experimental))
        {
            smoothaf_recovery_begin(&smoothaf_vlog_af.recovery, sample.score,
                                    smoothaf_vlog_af.direction);
            smoothaf_vlog_af_reserve_recovery(&smoothaf_vlog_af, 1);
            smoothaf_vlog_af.state = SMOOTHAF_VLOG_AF_PROBING;
            recovery_active = 1;
        }
        if (!result_processed && smoothaf_vlog_af.armed &&
            smoothaf_vlog_af_timeout_due(&smoothaf_vlog_af, completed))
        {
            smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_TIMEOUT,
                                  SMOOTHAF_VLOG_AF_BLOCK_NONE);
            result_processed = 1;
        }
        if (!result_processed && smoothaf_vlog_af.armed && smoothaf_vlog_af.state == SMOOTHAF_VLOG_AF_PROBING &&
            !smoothaf_vlog_af.awaiting_result)
        {
            gate_reason = smoothaf_vlog_af_command_block_reason_for(
                &smoothaf_vlog_af, smoothaf_vlog_af_mode,
                smoothaf_enabled, completed);
            if (gate_reason != SMOOTHAF_VLOG_AF_BLOCK_NONE)
            {
                smoothaf_af_command_block = gate_reason;
                if (gate_reason == SMOOTHAF_VLOG_AF_BLOCK_DISABLED)
                    smoothaf_last_block = SMOOTHAF_BLOCK_DISABLED;
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_BLOCKED,
                                      gate_reason);
            }
            else if (smoothaf_vlog_af_uses_dry_run(&smoothaf_vlog_af,
                                                    smoothaf_vlog_af_mode))
            {
                smoothaf_vlog_af_dry_run_request(&smoothaf_vlog_af, smoothaf_enabled, completed);
                smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
                smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_DRY_RUN;
                if (smoothaf_persist_logger_is_enabled(&smoothaf_persist_log))
                    smoothaf_persist_log_command(&smoothaf_persist_log, completed,
                                                 (uint16_t)smoothaf_af_episode_id,
                                                 (uint8_t)smoothaf_vlog_af.commands,
                                                 (int8_t)smoothaf_vlog_af.direction,
                                                 (uint8_t)smoothaf_vlog_af.command_step,
                                                 0, 0, 0);
            }
            else if (smoothaf_block_reason())
            {
                smoothaf_last_block = smoothaf_block_reason();
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_BLOCKED,
                                      SMOOTHAF_VLOG_AF_BLOCK_SAFETY_GATE);
            }
            else if (recovery_active &&
                     !smoothaf_recovery_next(&smoothaf_vlog_af.recovery,
                                             &recovery_direction, &recovery_step))
            {
                recovery_active = 0;
                smoothaf_vlog_af_reserve_recovery(&smoothaf_vlog_af, 0);
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT,
                                      SMOOTHAF_VLOG_AF_BLOCK_NONE);
            }
            else if (recovery_active)
            {
                command_direction = recovery_direction;
                command_step = recovery_step;
                if (smoothaf_episode_cancel_requested())
                {
                    smoothaf_vlog_controller_rollback_recovery();
                    smoothaf_episode_cancel();
                }
                else if (!smoothaf_move_with_step(command_direction, command_step))
                {
                    smoothaf_vlog_controller_rollback_recovery();
                    if (smoothaf_episode_cancel_requested())
                        smoothaf_episode_cancel();
                    else
                        smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_FAILED,
                                              SMOOTHAF_VLOG_AF_BLOCK_NONE);
                }
                else
                {
                    smoothaf_recovery_commit_move(&smoothaf_vlog_af.recovery);
                    smoothaf_vlog_af.direction = command_direction;
                    smoothaf_vlog_af.command_step = command_step;
                    smoothaf_vlog_af_record_command(&smoothaf_vlog_af,
                                                    (uint32_t)get_ms_clock());
                    smoothaf_vlog_af.current_position =
                        smoothaf_vlog_af.recovery.position;
                    smoothaf_af_commands_episode =
                        (int)smoothaf_vlog_af.commands;
                    smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                    smoothaf_af_last_direction = command_direction;
                    if (smoothaf_episode_cancel_requested())
                        smoothaf_episode_cancel();
                }
            }
            else if (!smoothaf_vlog_controller_next(&command_direction, &command_step))
            {
                smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT,
                                      SMOOTHAF_VLOG_AF_BLOCK_NONE);
            }
            else
            {
                if (smoothaf_episode_cancel_requested())
                {
                    smoothaf_vlog_controller_rollback_move(command_direction,
                                                           command_step);
                    smoothaf_episode_cancel();
                }
                else if (!smoothaf_move_with_step(command_direction, command_step))
                {
                    smoothaf_vlog_controller_rollback_move(command_direction,
                                                           command_step);
                    if (smoothaf_episode_cancel_requested())
                        smoothaf_episode_cancel();
                    else
                        smoothaf_vlog_af_fail(SMOOTHAF_VLOG_AF_RESULT_FAILED,
                                              SMOOTHAF_VLOG_AF_BLOCK_NONE);
                }
                else
                {
                    /* lens_focus() was accepted; commit the adapter position before
                     * observing a cancellation that may have arrived during the call. */
                    smoothaf_episode_commit_move(
                        &smoothaf_vlog_af, command_direction, command_step,
                        (uint32_t)get_ms_clock());
                    smoothaf_af_commands_episode = (int)smoothaf_vlog_af.commands;
                    smoothaf_af_current_position = smoothaf_vlog_af.current_position;
                    smoothaf_af_last_direction = smoothaf_vlog_af.direction;
                    if (smoothaf_episode_cancel_requested())
                    {
                        smoothaf_episode_cancel();
                        smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
                    }
                }
            }
        }
        smoothaf_monitor_log_transitions();
        /* Advance COOLDOWN only after this pass.  The state change itself never
         * commands the lens; a later monitor pass may act on PROBING. */
        smoothaf_vlog_af_advance_cooldown(&smoothaf_vlog_af,
                                          (uint32_t)get_ms_clock());

        /* FIO is serviced by the independent logger task. The AF task only
         * publishes fixed-size RAM records and never performs card I/O. */
    }
    smoothaf_cancel_pending_measure();
    if (smoothaf_af_episode_id && !smoothaf_af_episode_complete)
        smoothaf_episode_stop();
    if (smoothaf_af_episode_summary_pending && !smoothaf_af_episode_complete)
        smoothaf_episode_summary_publish(
            smoothaf_episode_result_map(smoothaf_vlog_af.episode_result),
            (uint32_t)get_ms_clock());

    smoothaf_persist_session_end(smoothaf_cancel ?
                                 (smoothaf_panic ? 3 : 2) : 1);

    smoothaf_persist_logger_sync_config();
    smoothaf_monitor_task_requested = 0;
}

static void smoothaf_measure_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    int score = -1;
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    int reason = smoothaf_measure_block_reason();
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    smoothaf_last_measure_code = SMOOTHAF_MEASURE_WAITING;
    smoothaf_last_measure_valid = 0;
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_publish_measurement(-1, 0, 0, 0, 0, SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = (reason == SMOOTHAF_BLOCK_CANCELLED ||
                           reason == SMOOTHAF_BLOCK_DISABLED)
                        ? SMOOTHAF_STATUS_CANCELLED : SMOOTHAF_STATUS_ERROR;
    }
    else if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
    }
    else
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
        smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                     smoothaf_has_confidence(score, mean)
                                     ? SMOOTHAF_MEASURE_OK : SMOOTHAF_MEASURE_LOW);
        smoothaf_status = smoothaf_has_confidence(score, mean)
                        ? SMOOTHAF_STATUS_IDLE : SMOOTHAF_STATUS_NO_CONFIDENCE;
    }
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
}

static void smoothaf_step_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    int score = -1;
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    int reason;
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        goto done;
    }
    if (!smoothaf_move(smoothaf_direction_value()))
    {
        smoothaf_last_error = 1;
        smoothaf_status = smoothaf_last_block ? SMOOTHAF_STATUS_ERROR
                                              : SMOOTHAF_STATUS_LENS_ERROR;
        goto done;
    }
    if (smoothaf_cancel)
    {
        smoothaf_status = SMOOTHAF_STATUS_CANCELLED;
        goto done;
    }
    msleep(SMOOTHAF_SETTLE_MS);
    if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        goto done;
    }
    smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                 smoothaf_has_confidence(score, mean)
                                 ? SMOOTHAF_MEASURE_OK : SMOOTHAF_MEASURE_LOW);
    smoothaf_status = smoothaf_has_confidence(score, mean)
                    ? SMOOTHAF_STATUS_IDLE : SMOOTHAF_STATUS_NO_CONFIDENCE;
done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
}

static enum smoothaf_result smoothaf_normal_run(struct smoothaf_normal_af *normal,
                                                int initial_score, uint32_t *start)
{
    enum smoothaf_normal_action action;
    int direction;
    int step;
    int score;
    int stop;
    smoothaf_normal_af_init(normal, initial_score, smoothaf_search_direction,
                            smoothaf_test_step);
    action = smoothaf_normal_af_begin(normal);
    for (;;)
    {
        if (action == SMOOTHAF_NORMAL_LOCKED) return SMOOTHAF_RESULT_LOCKED;
        if (action == SMOOTHAF_NORMAL_LIMIT) return SMOOTHAF_RESULT_LIMIT;
        if (action == SMOOTHAF_NORMAL_NO_CONFIDENCE)
            return SMOOTHAF_RESULT_NO_CONFIDENCE;
        stop = smoothaf_search_abort(start);
        if (stop) return (enum smoothaf_result)stop;
        if (action == SMOOTHAF_NORMAL_STEP)
        {
            if (!smoothaf_normal_af_next_step(normal, &direction, &step) ||
                step != smoothaf_test_step) return SMOOTHAF_RESULT_LIMIT;
            if (!smoothaf_search_move(0, direction)) return SMOOTHAF_RESULT_LENS_ERROR;
            stop = smoothaf_search_abort(start);
            if (stop) return (enum smoothaf_result)stop;
        }
        if (!smoothaf_search_measure(0, &score)) return SMOOTHAF_RESULT_MEASURE_ERROR;
        stop = smoothaf_search_abort(start);
        if (stop) return (enum smoothaf_result)stop;
        action = smoothaf_normal_af_on_score(normal, score);
    }
}

static enum smoothaf_result smoothaf_adaptive_run(
        struct smoothaf_adaptive_hc *adaptive, int initial_score, uint32_t *start)
{
    enum smoothaf_adaptive_action action;
    struct smoothaf_adaptive_policy policy;
    int direction;
    int abstract_step;
    int score;
    int stop;
    smoothaf_adaptive_policy_default(&policy);
    policy.min_step = smoothaf_test_step;
    policy.initial_step = smoothaf_test_step;
    policy.max_step = smoothaf_test_step;
    smoothaf_adaptive_hc_init(adaptive, &policy, initial_score,
                              smoothaf_search_direction);
    action = smoothaf_adaptive_hc_begin(adaptive);
    for (;;)
    {
        if (action == SMOOTHAF_ADAPTIVE_LOCKED) return SMOOTHAF_RESULT_LOCKED;
        if (action == SMOOTHAF_ADAPTIVE_NO_PROGRESS ||
            action == SMOOTHAF_ADAPTIVE_LIMIT) return SMOOTHAF_RESULT_LIMIT;
        if (action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE)
            return SMOOTHAF_RESULT_NO_CONFIDENCE;
        stop = smoothaf_search_abort(start);
        if (stop) return (enum smoothaf_result)stop;
        if (action == SMOOTHAF_ADAPTIVE_STEP)
        {
            if (!smoothaf_adaptive_hc_next_step(adaptive, &direction,
                                                &abstract_step))
                return SMOOTHAF_RESULT_LIMIT;
            (void)abstract_step;
            if (!smoothaf_search_move(0, direction)) return SMOOTHAF_RESULT_LENS_ERROR;
            stop = smoothaf_search_abort(start);
            if (stop) return (enum smoothaf_result)stop;
        }
        if (!smoothaf_search_measure(0, &score)) return SMOOTHAF_RESULT_MEASURE_ERROR;
        stop = smoothaf_search_abort(start);
        if (stop) return (enum smoothaf_result)stop;
        action = smoothaf_adaptive_hc_on_score(adaptive, score);
    }
}

static void smoothaf_search_task(void)
{
    uint32_t start = (uint32_t)get_ms_clock();
    struct smoothaf_adaptive_hc adaptive;
    struct smoothaf_normal_af normal;
    struct smoothaf_hc hc;
    int score = -1;
    int mean = 0;
    int sad = 0;
    int laplacian = 0;
    int reason;
    int use_adaptive = smoothaf_af_mode == SMOOTHAF_HC_ADAPTIVE;
    int use_normal = smoothaf_af_mode == SMOOTHAF_AF_NORMAL;
    enum smoothaf_result result = SMOOTHAF_RESULT_ERROR;
    smoothaf_test_session = 1;
    smoothaf_last_algorithm_valid = 0;
    smoothaf_last_algorithm_status = SMOOTHAF_STATUS_MEASURING;
    smoothaf_search_direction = smoothaf_test_direction_value();
    smoothaf_hc_init(&hc, 0, smoothaf_search_direction);
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    smoothaf_last_commands = 0;
    smoothaf_last_reversals = 0;
    smoothaf_test_auto_result = SMOOTHAF_AUTO_INVALID;
    smoothaf_test_auto_commands = 0;
    smoothaf_test_auto_reversals = 0;
    smoothaf_test_auto_first_score = score;
    smoothaf_test_auto_opposite_score = 0;
    smoothaf_test_auto_first_delta = 0;
    smoothaf_test_auto_opposite_delta = 0;
    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        goto done;
    }
    if (!smoothaf_measure_test(&score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        goto done;
    }
    smoothaf_publish_measurement(score, mean, sad, laplacian, 1,
                                 smoothaf_has_confidence(score, mean)
                                 ? SMOOTHAF_MEASURE_OK : SMOOTHAF_MEASURE_LOW);
    if (!smoothaf_has_confidence(score, mean))
    {
        smoothaf_status = SMOOTHAF_STATUS_NO_CONFIDENCE;
        goto done;
    }
    if (smoothaf_test_direction == SMOOTHAF_TEST_DIRECTION_AUTO)
    {
        struct smoothaf_auto_probe_config probe_config;
        struct smoothaf_auto_probe probe;
        /* The probe is complete before the selected controller starts. */
        probe_config.first_direction = -1;
        probe_config.initial_score = score;
        probe_config.max_commands = SMOOTHAF_AUTO_MAX_COMMANDS;
        probe_config.max_reversals = SMOOTHAF_AUTO_MAX_REVERSALS;
        smoothaf_test_auto_result = smoothaf_auto_probe_run(
            &probe, &probe_config, smoothaf_auto_move, smoothaf_auto_measure,
            smoothaf_auto_abort, &start);
        smoothaf_test_auto_commands = probe.commands;
        smoothaf_test_auto_reversals = probe.reversals;
        smoothaf_test_auto_first_score = probe.first_score;
        smoothaf_test_auto_opposite_score = probe.opposite_score;
        smoothaf_test_auto_first_delta = probe.first_delta;
        smoothaf_test_auto_opposite_delta = probe.opposite_delta;
        smoothaf_last_commands = probe.commands;
        smoothaf_last_reversals = probe.reversals;
        if (smoothaf_test_auto_result == SMOOTHAF_AUTO_ABORTED)
        {
            result = smoothaf_cancel ? SMOOTHAF_RESULT_CANCELLED : SMOOTHAF_RESULT_ERROR;
            goto done;
        }
        if (probe.selected_direction == 0)
        {
            result = smoothaf_test_auto_result == SMOOTHAF_AUTO_LOW_CONFIDENCE
                   ? SMOOTHAF_RESULT_NO_CONFIDENCE : SMOOTHAF_RESULT_LIMIT;
            goto done;
        }
        smoothaf_search_direction = probe.selected_direction;
    }
    if (use_normal)
    {
        result = smoothaf_normal_run(&normal, score, &start);
        smoothaf_last_mode = SMOOTHAF_AF_NORMAL;
        smoothaf_last_phase = normal.phase;
        smoothaf_last_commands = normal.commands;
        smoothaf_last_reversals = normal.reversals;
        smoothaf_last_best = normal.best;
        smoothaf_last_focus_pos = normal.best_pos;
        smoothaf_last_algorithm_focus_pos = normal.best_pos;
        smoothaf_last_score = normal.current;
    }
    else if (use_adaptive)
    {
        result = smoothaf_adaptive_run(&adaptive, score, &start);
        smoothaf_last_mode = SMOOTHAF_HC_ADAPTIVE;
        smoothaf_last_phase = adaptive.phase;
        smoothaf_last_commands = adaptive.commands;
        smoothaf_last_reversals = adaptive.reversals;
        smoothaf_last_best = adaptive.best;
        smoothaf_last_focus_pos = adaptive.best_pos;
        smoothaf_last_algorithm_focus_pos = adaptive.best_pos;
        smoothaf_last_score = adaptive.current;
    }
    else
    {
        result = smoothaf_hc_run(&hc, score, smoothaf_search_direction,
                                 smoothaf_search_move, smoothaf_search_measure,
                                 smoothaf_search_abort, &start);
        smoothaf_last_mode = SMOOTHAF_HC_LEGACY;
        smoothaf_last_phase = hc.local_max ? 2 : 0;
        smoothaf_last_commands = hc.commands;
        smoothaf_last_reversals = hc.reversals;
        smoothaf_last_best = hc.best;
        smoothaf_last_focus_pos = hc.best_pos;
        smoothaf_last_algorithm_focus_pos = hc.best_pos;
        smoothaf_last_score = hc.current;
    }
    smoothaf_status = smoothaf_status_from_result(result);
    smoothaf_last_algorithm_valid = 1;
    smoothaf_last_algorithm_status = smoothaf_status;
    smoothaf_physical_movement_state = result == SMOOTHAF_RESULT_LENS_ERROR
                                    ? SMOOTHAF_MOVEMENT_FAILED
                                    : SMOOTHAF_MOVEMENT_COMPLETE;
    if (result == SMOOTHAF_RESULT_LENS_ERROR ||
        result == SMOOTHAF_RESULT_MEASURE_ERROR ||
        result == SMOOTHAF_RESULT_TIMEOUT || result == SMOOTHAF_RESULT_ERROR)
        smoothaf_last_error = 1;
done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
    if (result == SMOOTHAF_RESULT_LENS_ERROR || result == SMOOTHAF_RESULT_MEASURE_ERROR ||
        result == SMOOTHAF_RESULT_ERROR)
        smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_FAILED;
    smoothaf_log_event_extended(SMOOTHAF_LOG_TASK_END, result,
                                smoothaf_last_best, smoothaf_last_focus_pos,
                                smoothaf_last_commands, smoothaf_last_reversals);
    smoothaf_telemetry_update(smoothaf_last_score, smoothaf_last_time,
                              smoothaf_search_direction, smoothaf_test_step,
                              smoothaf_status, smoothaf_last_error,
                              smoothaf_last_block);
    smoothaf_test_session = 0;
}

static void smoothaf_task_entry(void *arg)
{
    int task_id = (int)(intptr_t)arg;
    switch (task_id)
    {
        case SMOOTHAF_TASK_MEASURE: smoothaf_measure_task(); break;
        case SMOOTHAF_TASK_STEP: smoothaf_step_task(); break;
        case SMOOTHAF_TASK_SEARCH: smoothaf_search_task(); break;
        case SMOOTHAF_TASK_MONITOR: smoothaf_monitor_task(); break;
        default: break;
    }
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
        smoothaf_task_running = 0;
        smoothaf_task_stop_requested = 0;
        smoothaf_task_exited = 1;
    }
}

static void smoothaf_fio_task(void *arg)
{
    (void)arg;
    smoothaf_fio_task_running = 1;
    smoothaf_fio_task_exited = 0;
    while (!smoothaf_fio_shutdown_requested) {
        smoothaf_persist_logger_sync_config();
        if (smoothaf_log_fio_is_enabled(&smoothaf_persist_fio) ||
            smoothaf_persist_fio.config_pending ||
            smoothaf_persist_fio.close_pending)
            smoothaf_log_fio_service(&smoothaf_persist_fio,
                                     &smoothaf_persist_log,
                                     &smoothaf_persist_batch);
        smoothaf_persist_logger_record_fio_errors();
        msleep(20);
    }
    /* Shutdown is deliberately non-draining: STOP/deinit must not wait for
     * card I/O. The consumer remains the sole owner of FILE * and closes it
     * before publishing task exit. Any unflushed RAM records are retained only
     * in the disposable module state. */
    if (smoothaf_log_fio_is_enabled(&smoothaf_persist_fio) ||
        smoothaf_persist_fio.current_file)
        smoothaf_log_fio_set_enabled(&smoothaf_persist_fio, false);
    smoothaf_fio_task_running = 0;
    smoothaf_fio_task_exited = 1;
}

static int smoothaf_start_fio_task(void)
{
    struct task *task;
    if (smoothaf_fio_task_running) return 1;
    smoothaf_fio_task_created = 1;
    smoothaf_fio_task_exited = 0;
    task = task_create("smoothaf_fio", SMOOTHAF_FIO_TASK_PRIORITY,
                       SMOOTHAF_FIO_TASK_STACK, smoothaf_fio_task, NULL);
    if (!task || (((uintptr_t)task) & 1)) {
        smoothaf_fio_task_created = 0;
        smoothaf_fio_task_running = 0;
        smoothaf_fio_task_exited = 1;
        return 0;
    }
    return 1;
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
    return smoothaf_task_running;
}

static int smoothaf_start_task(int task_id, int requires_enable)
{
    struct task *task;
    if (!smoothaf_lock())
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_SEMAPHORE;
        smoothaf_last_error = 1;
        return 0;
    }
    if ((requires_enable && !smoothaf_can_start(smoothaf_enabled,
                                                 smoothaf_panic,
                                                 smoothaf_task_running)) ||
        (!requires_enable && smoothaf_task_running))
    {
        smoothaf_unlock();
        return 0;
    }
    smoothaf_task_running = 1;
    smoothaf_task_exited = 0;
    smoothaf_session_started_ms = (uint32_t)get_ms_clock();
    smoothaf_session_finished_ms = 0;
    smoothaf_cancel = 0;
    smoothaf_last_error = 0;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    smoothaf_last_lens_called = 0;
    smoothaf_last_lens_return = 0;
    smoothaf_unlock();
    task = task_create("smoothaf_task", SMOOTHAF_TASK_PRIORITY, SMOOTHAF_TASK_STACK,
                       smoothaf_task_entry, (void *)(intptr_t)task_id);
    if (!task || (((uintptr_t)task) & 1))
    {
        smoothaf_task_running = 0;
        smoothaf_task_exited = 1;
        smoothaf_session_finished_ms = (uint32_t)get_ms_clock();
        smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
        smoothaf_last_error = 1;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        return 0;
    }
    return 1;
}

/* A queued Measure must not stay WAITING after its consumer disappears. */
static void smoothaf_cancel_pending_measure(void)
{
    if (!smoothaf_measure_requested) return;
    smoothaf_measure_requested = 0;
    smoothaf_last_measure_code = SMOOTHAF_MEASURE_ERROR;
    smoothaf_last_measure_valid = 0;
    smoothaf_last_error = 1;
    smoothaf_last_block = SMOOTHAF_BLOCK_CANCELLED;
    smoothaf_status = SMOOTHAF_STATUS_CANCELLED;
}

static MENU_SELECT_FUNC(smoothaf_persist_logger_config_select)
{
    smoothaf_persist_logger_sync_config();
    if (smoothaf_logger_fio_enabled && smoothaf_logger_enabled &&
        !smoothaf_fio_task_running && !smoothaf_fio_task_created &&
        !smoothaf_monitor_deinit && !smoothaf_start_fio_task())
    {
        smoothaf_logger_fio_enabled = 0;
        smoothaf_last_error = 1;
        smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
    }
}

static MENU_SELECT_FUNC(smoothaf_measure_select)
{
    int route = smoothaf_measure_route_for(
        smoothaf_panic, smoothaf_is_task_running(),
        smoothaf_monitor_task_requested && smoothaf_monitor.enabled &&
        !smoothaf_task_stop_requested);
    if (route == SMOOTHAF_MEASURE_ROUTE_QUEUED)
    {
        smoothaf_measure_requested = 1;
        smoothaf_last_measure_code = SMOOTHAF_MEASURE_WAITING;
        smoothaf_last_measure_valid = 0;
        smoothaf_last_error = 0;
        smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
        smoothaf_status = SMOOTHAF_STATUS_MEASURING;
        return;
    }
    if (route == SMOOTHAF_MEASURE_ROUTE_BLOCKED)
    {
        smoothaf_last_block = smoothaf_panic ? SMOOTHAF_BLOCK_DISABLED
                                             : SMOOTHAF_BLOCK_BUSY;
        smoothaf_last_error = 1;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        return;
    }
    smoothaf_start_task(SMOOTHAF_TASK_MEASURE, 0);
}

static MENU_SELECT_FUNC(smoothaf_step_select)
{
    smoothaf_start_task(SMOOTHAF_TASK_STEP, 1);
}

static MENU_SELECT_FUNC(smoothaf_search_select)
{
    if (smoothaf_monitor.enabled || smoothaf_monitor_task_requested)
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_BUSY;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
        return;
    }
    smoothaf_start_task(SMOOTHAF_TASK_SEARCH, 1);
}

static MENU_SELECT_FUNC(smoothaf_monitor_toggle_select)
{
    if (smoothaf_monitor.enabled)
    {
        smoothaf_request_episode_stop();
        if (smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_NONE ||
            smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_ARMED)
            smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
        smoothaf_af_episode_reason = smoothaf_vlog_af.episode_result;
        smoothaf_cancel_pending_measure();
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
    smoothaf_monitor_start(&smoothaf_monitor, smoothaf_vlog_metric,
                           (uint32_t)get_ms_clock());
    smoothaf_session_started_ms = (uint32_t)get_ms_clock();
    smoothaf_persist_logger_sync_config();
    smoothaf_persist_session_start();
    smoothaf_vlog_af_reset_session(&smoothaf_vlog_af, smoothaf_vlog_af_mode);
    smoothaf_vlog_algorithm_apply_at_episode_boundary();
    smoothaf_vlog_af_set_algorithm(&smoothaf_vlog_af,
                                   smoothaf_vlog_algorithm_active);
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    smoothaf_af_commands_episode = 0;
    smoothaf_measure_requested = 0;
    smoothaf_monitor_task_requested = 1;
    smoothaf_monitor_deinit = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 0;
    smoothaf_cancel = 0;
    smoothaf_monitor_log_transitions();
    if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))
    {
        smoothaf_persist_session_end(4);
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_task_requested = 0;
        smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_ERROR);
        smoothaf_monitor_log_transitions();
    }
}

static const char *smoothaf_monitor_task_state_name(void)
{
    return smoothaf_monitor_task_state_name_for(
        smoothaf_task_running, smoothaf_task_stop_requested,
        smoothaf_task_exited, smoothaf_monitor.state == SMOOTHAF_MONITOR_ERROR);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_toggle_update)
{
    MENU_SET_VALUE("%s", smoothaf_monitor_task_state_name());
}

static MENU_UPDATE_FUNC(smoothaf_monitor_task_state_update)
{ MENU_SET_VALUE("%s", smoothaf_monitor_task_state_name()); }

static MENU_UPDATE_FUNC(smoothaf_monitor_configured_interval_update)
{
    MENU_SET_VALUE("%d ms", (int)smoothaf_monitor_interval_ms(smoothaf_monitor.interval_index));
    MENU_SET_HELP("Configured sampling interval; edit Sample Int in Vlog Monitor.");
}

static MENU_UPDATE_FUNC(smoothaf_monitor_actual_interval_update)
{
    MENU_SET_VALUE("%d ms", (int)smoothaf_monitor.metrics.actual_interval_ms);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_last_duration_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_monitor.metrics.duration_last); }

static MENU_UPDATE_FUNC(smoothaf_monitor_average_duration_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_monitor_duration_average(&smoothaf_monitor)); }

static MENU_UPDATE_FUNC(smoothaf_monitor_max_duration_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_monitor.metrics.duration_max); }

#define SMOOTHAF_MONITOR_COUNTER_UPDATE(name, field) \
static MENU_UPDATE_FUNC(name) \
{ MENU_SET_VALUE("%d", (int)smoothaf_monitor.metrics.field); }
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_attempts_update, attempts)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_valid_update, valid)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_invalid_update, invalid)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_low_confidence_update, low_confidence)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_blocked_update, blocked)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_skipped_update, skipped)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_overruns_update, overruns)
SMOOTHAF_MONITOR_COUNTER_UPDATE(smoothaf_monitor_error_update, last_error)
#undef SMOOTHAF_MONITOR_COUNTER_UPDATE

static MENU_UPDATE_FUNC(smoothaf_monitor_status_update)
{
    MENU_SET_VALUE("%s", smoothaf_monitor_state_name(smoothaf_monitor.state));
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
{ MENU_SET_VALUE("%d", smoothaf_monitor.sad); }
static MENU_UPDATE_FUNC(smoothaf_monitor_laplacian_update)
{ MENU_SET_VALUE("%d", smoothaf_monitor.laplacian); }
static MENU_UPDATE_FUNC(smoothaf_monitor_samples_update)
{ MENU_SET_VALUE("%d", smoothaf_monitor.metrics.attempts); }
static MENU_UPDATE_FUNC(smoothaf_monitor_smoothed_update)
{ MENU_SET_VALUE("%d", smoothaf_monitor.smoothed); }
static MENU_UPDATE_FUNC(smoothaf_monitor_reference_update)
{
    MENU_SET_VALUE("%d", smoothaf_monitor.reference);
    MENU_SET_HELP("state=%s src=%s",
                  smoothaf_monitor_reference_state_name(
                      smoothaf_monitor.reference_state),
                  smoothaf_monitor_reference_reason_name(
                      smoothaf_monitor.reference_reason));
}
static MENU_UPDATE_FUNC(smoothaf_monitor_blur_update)
{ MENU_SET_VALUE("%s", smoothaf_monitor.possible_blur ? "YES" : "NO"); }

static MENU_UPDATE_FUNC(smoothaf_movie_recording_update)
{
    MENU_SET_VALUE("%s %d/%d", smoothaf_movie_recording.active ? "REC" : "IDLE",
                   (int)smoothaf_movie_recording.valid,
                   (int)smoothaf_movie_recording.samples);
    MENU_SET_HELP("class=%d invalid=%d started=%ums ended=%ums",
                  smoothaf_movie_recording.class_id,
                  (int)smoothaf_movie_recording.invalid,
                  (unsigned)smoothaf_movie_recording.started_ms,
                  (unsigned)smoothaf_movie_recording.ended_ms);
}

static const char *smoothaf_vlog_af_mode_name(void)
{
    return smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS
             ? "ACTIVE CONTINUOUS" :
           smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE ? "ACTIVE" :
           smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_DRY_RUN ? "DRY RUN" : "OFF";
}

static const char *smoothaf_vlog_af_state_name_for(int state)
{
    switch (state)
    {
        case SMOOTHAF_VLOG_AF_PROBING: return "PROBING";
        case SMOOTHAF_VLOG_AF_SETTLING: return "SETTLING";
        case SMOOTHAF_VLOG_AF_COOLDOWN: return "COOLDOWN";
        case SMOOTHAF_VLOG_AF_COMPLETE: return "COMPLETE";
        case SMOOTHAF_VLOG_AF_ERROR: return "ERROR";
        default: return "WAITING";
    }
}

static const char *smoothaf_vlog_af_state_name(void)
{
    return smoothaf_vlog_af_state_name_for(smoothaf_vlog_af.state);
}

static const char *smoothaf_vlog_af_session_state_name(void)
{
    if (smoothaf_vlog_af.continuous_armed)
        return smoothaf_vlog_af.safety_paused ? "PAUSED" : "AUTHORIZED";
    return smoothaf_vlog_af.armed ? "EPISODE ACTIVE" : "INACTIVE";
}

static MENU_UPDATE_FUNC(smoothaf_vlog_af_mode_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_mode_name()); }

static MENU_UPDATE_FUNC(smoothaf_vlog_algorithm_update)
{
    /* Refresh the pending marker without changing an active episode. */
    smoothaf_vlog_algorithm_apply_at_episode_boundary();
    MENU_SET_VALUE("%s%s", smoothaf_vlog_algorithm_name(),
                   smoothaf_vlog_algorithm_pending_flag ? " PENDING" : "");
    MENU_SET_HELP("Active for the current/next safe episode: %s; invalid values fall back to Current Vlog.",
                  smoothaf_algorithm_name_for(smoothaf_vlog_algorithm_active));
}

static MENU_UPDATE_FUNC(smoothaf_vlog_recovery_update)
{
    MENU_SET_VALUE("%s", smoothaf_far_focus_recovery ? "ON" : "OFF");
    MENU_SET_HELP("phase=%s result=%s cmd=%d best=%d pos=%d moves=%d",
                  smoothaf_recovery_phase_name(smoothaf_vlog_af.recovery.phase),
                  smoothaf_recovery_result_name(smoothaf_vlog_af.recovery.result),
                  smoothaf_vlog_af.recovery.commands,
                  smoothaf_vlog_af.recovery.best_score,
                  smoothaf_vlog_af.recovery.position,
                  smoothaf_vlog_af.recovery.commands);
}

static MENU_UPDATE_FUNC(smoothaf_vlog_af_state_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_state_name()); }

static MENU_UPDATE_FUNC(smoothaf_vlog_af_session_state_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_session_state_name()); }

static MENU_UPDATE_FUNC(smoothaf_vlog_af_result_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_result_name(smoothaf_af_episode_result)); }

static MENU_UPDATE_FUNC(smoothaf_af_move_attempts_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_move_attempts); }

static MENU_UPDATE_FUNC(smoothaf_af_lens_calls_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_lens_calls); }

static MENU_UPDATE_FUNC(smoothaf_af_move_successes_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_move_successes); }

static MENU_UPDATE_FUNC(smoothaf_af_last_block_update)
{ MENU_SET_VALUE("%s", smoothaf_block_name(smoothaf_last_block)); }

static MENU_UPDATE_FUNC(smoothaf_af_last_move_result_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_last_lens_return); }

static MENU_UPDATE_FUNC(smoothaf_af_command_block_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_block_name(smoothaf_af_command_block)); }

static MENU_UPDATE_FUNC(smoothaf_af_episode_reason_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_result_name(smoothaf_af_episode_reason)); }

static MENU_UPDATE_FUNC(smoothaf_af_episode_state_before_error_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_state_name_for(smoothaf_af_episode_state_before_error)); }

static MENU_UPDATE_FUNC(smoothaf_af_post_move_valid_update)
{ MENU_SET_VALUE("%s", smoothaf_af_post_move_valid ? "YES" : "NO"); }

static MENU_UPDATE_FUNC(smoothaf_af_last_result_valid_update)
{ MENU_SET_VALUE("%s", smoothaf_af_last_result_valid ? "YES" : "NO"); }

static MENU_UPDATE_FUNC(smoothaf_af_commands_episode_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_af_commands_episode); }

static MENU_UPDATE_FUNC(smoothaf_af_auto_probe_commands_update)
{ MENU_SET_VALUE("%d", smoothaf_vlog_af.auto_probe_commands); }

static MENU_UPDATE_FUNC(smoothaf_af_algorithm_commands_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_vlog_af.algorithm_commands); }

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

static MENU_UPDATE_FUNC(smoothaf_af_continuous_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af.continuous_armed ? "YES" : "NO"); }

static MENU_UPDATE_FUNC(smoothaf_af_next_block_update)
{
    int reason = smoothaf_vlog_af_next_episode_block_reason_for(
        &smoothaf_vlog_af, smoothaf_monitor.state,
        smoothaf_monitor.reference_state, smoothaf_monitor.reference_epoch,
        smoothaf_vlog_af_mode, smoothaf_enabled, (uint32_t)get_ms_clock());
    MENU_SET_VALUE("%s", smoothaf_vlog_af_next_block_name(reason));
}

static MENU_UPDATE_FUNC(smoothaf_af_recheck_update)
{
    uint32_t now = (uint32_t)get_ms_clock();
    if (!smoothaf_vlog_af.wait_for_blur_clear)
        MENU_SET_VALUE("CLEAR");
    else if ((int32_t)(now - smoothaf_vlog_af.blur_recheck_until_ms) >= 0)
        MENU_SET_VALUE("DUE");
    else
        MENU_SET_VALUE("%d ms", (int)(smoothaf_vlog_af.blur_recheck_until_ms - now));
}

static MENU_UPDATE_FUNC(smoothaf_af_movement_update)
{ MENU_SET_VALUE("%s", smoothaf_movement_state_name()); }

static int smoothaf_vlog_af_arm_block_reason(void)
{
    int reason = smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_DRY_RUN
               ? smoothaf_measure_block_reason() : smoothaf_block_reason();
    if ((smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE ||
         smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) &&
        !smoothaf_movie_active_recording_allowed(
            smoothaf_vlog_af_mode, smoothaf_movie_active_experimental, lv,
            is_movie_mode(), RECORDING_H264, RECORDING_CUSTOM, RECORDING_RAW) &&
        is_movie_mode())
        return SMOOTHAF_BLOCK_MOVIE;
    if (reason) return reason;
    if (is_manual_focus()) return SMOOTHAF_BLOCK_MANUAL_FOCUS;
    if (is_continuous_af()) return SMOOTHAF_BLOCK_CONTINUOUS_AF;
    if (is_follow_focus_active()) return SMOOTHAF_BLOCK_FOLLOW_FOCUS;
    if (is_rack_focus_enabled()) return SMOOTHAF_BLOCK_RACK_FOCUS;
    if (is_focus_stack_enabled()) return SMOOTHAF_BLOCK_FOCUS_STACK;
    if (smoothaf_vlog_step < 1 || smoothaf_vlog_step > 3) return SMOOTHAF_BLOCK_PARAMS;
    return SMOOTHAF_BLOCK_NONE;
}

static MENU_SELECT_FUNC(smoothaf_vlog_af_episode_select)
{
    int reason;
    if ((smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE ||
         smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) &&
        !smoothaf_enabled)
        return;
    if (smoothaf_vlog_af.continuous_armed)
        return;
    if (!smoothaf_vlog_af_can_arm(smoothaf_vlog_af_mode,
                                  smoothaf_monitor.enabled,
                                  smoothaf_is_task_running(),
                                  smoothaf_monitor_task_requested,
                                  smoothaf_vlog_af.armed))
        return;
    if (smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE &&
        smoothaf_vlog_af.session_commands >=
            smoothaf_vlog_af_session_command_limit_for(
                &smoothaf_vlog_af, smoothaf_vlog_direction_auto_selected()))
    {
        smoothaf_af_command_block = SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT;
        smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
        smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
        return;
    }
    reason = smoothaf_vlog_af_arm_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_af_command_block = SMOOTHAF_VLOG_AF_BLOCK_NONE;
        smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
        smoothaf_af_episode_state_before_error = smoothaf_vlog_af.state;
        smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_BLOCKED;
        return;
    }
    smoothaf_vlog_algorithm_start_episode();
    if (smoothaf_vlog_af_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS)
    {
        smoothaf_vlog_af_arm_continuous(&smoothaf_vlog_af,
                                        smoothaf_vlog_direction_for_episode(),
                                        smoothaf_monitor.score);
        smoothaf_vlog_af_set_auto_probe(&smoothaf_vlog_af,
                                        smoothaf_vlog_direction_auto_selected());
        smoothaf_vlog_af_set_episode_limit(&smoothaf_vlog_af,
                                           smoothaf_vlog_episode_limit);
    }
    else
    {
        smoothaf_vlog_af_new_episode(&smoothaf_vlog_af, smoothaf_vlog_af_mode,
                                     smoothaf_vlog_direction_for_episode(),
                                     smoothaf_monitor.score);
        smoothaf_vlog_af_set_auto_probe(&smoothaf_vlog_af,
                                        smoothaf_vlog_direction_auto_selected());
        smoothaf_vlog_af_set_episode_limit(&smoothaf_vlog_af,
                                           smoothaf_vlog_episode_limit);
    }
    smoothaf_episode_begin(SMOOTHAF_EPISODE_EVENT_ARM);
    smoothaf_af_commands_episode = 0;
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_ARMED;
    smoothaf_af_episode_reason = SMOOTHAF_VLOG_AF_RESULT_ARMED;
    smoothaf_af_command_block = SMOOTHAF_VLOG_AF_BLOCK_NONE;
    smoothaf_af_episode_state_before_error = SMOOTHAF_VLOG_AF_WAITING;
    smoothaf_af_post_move_valid = 0;
    smoothaf_af_last_result_valid = 0;
    smoothaf_af_initial_score = smoothaf_monitor.score;
    smoothaf_af_post_move_score = 0;
    smoothaf_af_best_score = smoothaf_monitor.score;
    smoothaf_af_score_delta = 0;
    smoothaf_af_best_position = 0;
    smoothaf_af_current_position = 0;
    smoothaf_af_last_direction = smoothaf_af_direction_value();
}

static MENU_SELECT_FUNC(smoothaf_diag_previous_select)
{
    smoothaf_diag_refresh();
    if (smoothaf_diag_page) smoothaf_diag_page--;
}

static MENU_SELECT_FUNC(smoothaf_diag_next_select)
{
    smoothaf_diag_refresh();
    if (smoothaf_diag_page + 1 < smoothaf_diag_count) smoothaf_diag_page++;
}

static MENU_UPDATE_FUNC(smoothaf_episode_summary_update)
{
    if (!smoothaf_lock())
    {
        MENU_SET_VALUE("BUSY");
        return;
    }
    smoothaf_episode_summary_count = smoothaf_episode_summaries_snapshot(
        &smoothaf_episode_log, smoothaf_episode_summary_snapshot,
        SMOOTHAF_EPISODE_SUMMARY_CAPACITY);
    smoothaf_unlock();
    if (!smoothaf_episode_summary_count)
    {
        MENU_SET_VALUE("EMPTY");
        return;
    }
    if (smoothaf_episode_summary_page >= smoothaf_episode_summary_count)
        smoothaf_episode_summary_page = smoothaf_episode_summary_count - 1;
    if (smoothaf_episode_summary_seen_count != smoothaf_episode_summary_count)
    {
        smoothaf_episode_summary_page = smoothaf_episode_summary_count - 1;
        smoothaf_episode_summary_seen_count = smoothaf_episode_summary_count;
    }
    MENU_SET_VALUE("%d/%d id=%u %s", (int)(smoothaf_episode_summary_page + 1),
                   (int)smoothaf_episode_summary_count,
                   (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].episode_id,
                   smoothaf_episode_result_name(smoothaf_episode_summary_snapshot[
                       smoothaf_episode_summary_page].result));
    MENU_SET_HELP("best=%d final=%d img=%ums wait=%ums move=%ums settle=%ums lens=%ums wait2=%ums smpw=%ums cmds=%u rev=%u",
                  (int)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].best_score,
                  (int)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].final_score,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].image_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].wait_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].movement_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].stabilization_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].total_lens_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].total_settle_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].total_sample_wait_ms,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].commands,
                  (unsigned)smoothaf_episode_summary_snapshot[smoothaf_episode_summary_page].reversals);
}

static MENU_SELECT_FUNC(smoothaf_episode_summary_previous_select)
{
    if (smoothaf_episode_summary_page) smoothaf_episode_summary_page--;
}

static MENU_SELECT_FUNC(smoothaf_episode_summary_next_select)
{
    if (smoothaf_episode_summary_page + 1 < smoothaf_episode_summary_count)
        smoothaf_episode_summary_page++;
}

static MENU_SELECT_FUNC(smoothaf_diag_clear_select)
{
    if (smoothaf_is_task_running() || smoothaf_monitor.enabled)
        return;
    if (smoothaf_lock())
    {
        smoothaf_log_clear(&smoothaf_log);
        smoothaf_episode_log_clear(&smoothaf_episode_log);
        smoothaf_diag_count = 0;
        smoothaf_diag_page = 0;
        smoothaf_diag_seen_total = 0;
        smoothaf_episode_summary_count = 0;
        smoothaf_episode_summary_page = 0;
        smoothaf_unlock();
    }
}

static MENU_UPDATE_FUNC(smoothaf_diag_page_update)
{
    smoothaf_diag_refresh();
    MENU_SET_VALUE("%d/%d", (int)(smoothaf_diag_count ? smoothaf_diag_page + 1 : 0),
                   (int)smoothaf_diag_count);
    MENU_SET_HELP("Logging %s; newest record selected automatically.",
                  smoothaf_log.enabled ? "ON" : "OFF");
}

static MENU_UPDATE_FUNC(smoothaf_diag_entry_update)
{
    struct smoothaf_log_entry *record;
    smoothaf_diag_refresh();
    if (!smoothaf_diag_count)
    {
        MENU_SET_VALUE("EMPTY");
        MENU_SET_HELP("No records. Enable Debug Logging before an action.");
        return;
    }
    record = &smoothaf_diag_snapshot[smoothaf_diag_page];
    MENU_SET_VALUE("%s t=%ums", smoothaf_log_type_name(record->type),
                   (unsigned)record->timestamp_ms);
    switch (record->type)
    {
        case SMOOTHAF_LOG_MOVE:
            MENU_SET_HELP("dir=%d step=%d score=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3);
            break;
        case SMOOTHAF_LOG_MEASURE:
            MENU_SET_HELP("score=%d mean=%d sad=%d lap=%d code=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        case SMOOTHAF_LOG_CONTROLLER:
            MENU_SET_HELP("action=%d best=%d bpos=%d pos=%d dir=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        case SMOOTHAF_LOG_ABORT:
            MENU_SET_HELP("result=%d dir=%d step=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3);
            break;
        case SMOOTHAF_LOG_ERROR:
            MENU_SET_HELP("code=%d mean=%d sad=%d lap=%d", (int)record->value1,
                          (int)record->value2, (int)record->value3,
                          (int)record->value4);
            break;
        case SMOOTHAF_LOG_TASK_END:
            MENU_SET_HELP("result=%d best=%d pos=%d cmds=%d rev=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        case SMOOTHAF_LOG_MONITOR:
            MENU_SET_HELP("from=%d to=%d ref=%d smooth=%d valid=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        case SMOOTHAF_LOG_RECORDING:
            MENU_SET_HELP("edge/sample=%d class/score=%d duration=%d confidence=%d state=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
        default:
            MENU_SET_HELP("v1=%d v2=%d v3=%d v4=%d v5=%d",
                          (int)record->value1, (int)record->value2,
                          (int)record->value3, (int)record->value4,
                          (int)record->value5);
            break;
    }
}

static MENU_UPDATE_FUNC(smoothaf_diag_dropped_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_log.dropped); }

static MENU_UPDATE_FUNC(smoothaf_logger_fill_update)
{ MENU_SET_VALUE("%d / %d", (int)smoothaf_persist_log_get_fill(&smoothaf_persist_log), SMOOTHAF_LOG_BUF_ENTRIES - 1); }

static MENU_UPDATE_FUNC(smoothaf_logger_dropped_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_persist_log_get_dropped(&smoothaf_persist_log)); }

static MENU_UPDATE_FUNC(smoothaf_logger_fio_errors_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_log_fio_get_write_errors(&smoothaf_persist_fio)); }

static MENU_UPDATE_FUNC(smoothaf_logger_fio_partials_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_log_fio_get_partial_writes(&smoothaf_persist_fio)); }

static MENU_UPDATE_FUNC(smoothaf_logger_fio_files_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_persist_fio.total_files_written); }

static MENU_UPDATE_FUNC(smoothaf_measure_result_update)
{
    switch (smoothaf_last_measure_code)
    {
        case SMOOTHAF_MEASURE_OK:
            MENU_SET_VALUE("OK score=%d", (int)smoothaf_last_score);
            break;
        case SMOOTHAF_MEASURE_LOW:
            MENU_SET_VALUE("LOW score=%d", (int)smoothaf_last_score);
            break;
        case SMOOTHAF_MEASURE_ERROR:
            MENU_SET_VALUE("ERROR %s", smoothaf_block_name(smoothaf_last_block));
            break;
        default:
            MENU_SET_VALUE("WAITING");
            break;
    }
    MENU_SET_HELP("mean=%d sad=%d lap=%d blk=%s t=%dms",
                  (int)smoothaf_last_mean, (int)smoothaf_last_sad,
                  (int)smoothaf_last_laplacian,
                  smoothaf_block_name(smoothaf_last_block),
                  (int)smoothaf_last_time);
}

static MENU_UPDATE_FUNC(smoothaf_vlog_af_commands_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_vlog_af.session_commands); }

static MENU_UPDATE_FUNC(smoothaf_vlog_af_available_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_vlog_af_commands_available(&smoothaf_vlog_af)); }

static MENU_UPDATE_FUNC(smoothaf_vlog_af_last_command_update)
{
    uint32_t now = (uint32_t)get_ms_clock();
    MENU_SET_VALUE("%d ms", smoothaf_vlog_af.last_command_ms
                   ? (int)(now - smoothaf_vlog_af.last_command_ms) : -1);
}

static MENU_UPDATE_FUNC(smoothaf_vlog_af_timeout_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_vlog_af_timeout_remaining(
                              &smoothaf_vlog_af, (uint32_t)get_ms_clock())); }

static MENU_UPDATE_FUNC(smoothaf_af_episodes_completed_update)
{ MENU_SET_VALUE("%d", (int)smoothaf_vlog_af.episodes_completed); }

static MENU_UPDATE_FUNC(smoothaf_af_consecutive_failures_update)
{ MENU_SET_VALUE("%d", smoothaf_vlog_af.consecutive_failures); }

static MENU_UPDATE_FUNC(smoothaf_af_cooldown_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_vlog_af_cooldown_remaining(
                               &smoothaf_vlog_af, (uint32_t)get_ms_clock())); }

static const char *smoothaf_algorithm_name(void)
{
    return smoothaf_algorithm_name_for(smoothaf_test_session
                                       ? smoothaf_af_mode
                                       : smoothaf_vlog_algorithm_active);
}

static const char *smoothaf_algorithm_phase_name(void)
{
    if (!smoothaf_test_session && smoothaf_vlog_af.armed)
        return smoothaf_vlog_af_state_name();
    if (!smoothaf_test_session || !smoothaf_last_algorithm_valid) return "N/A";
    switch (smoothaf_last_phase)
    {
        case 1: return "ACQUISITION";
        case 2: return "REFINEMENT";
        case 3: return "CONFIRM";
        case 4: return "RETURN";
        default: return "SEARCH";
    }
}

static const char *smoothaf_movement_state_name(void)
{
    if (smoothaf_test_session)
    {
        if (smoothaf_status == SMOOTHAF_STATUS_MEASURING) return "TEST MEASURING";
        if (smoothaf_status == SMOOTHAF_STATUS_MOVING) return "TEST MOVING";
    }
    if (smoothaf_vlog_af.awaiting_result)
        return "POST-MOVE PENDING";
    switch (smoothaf_physical_movement_state)
    {
        case SMOOTHAF_MOVEMENT_MOVING: return "MOVING";
        case SMOOTHAF_MOVEMENT_COMPLETE: return "COMPLETE";
        case SMOOTHAF_MOVEMENT_FAILED: return "FAILED";
        default: return "IDLE";
    }
}

static MENU_UPDATE_FUNC(smoothaf_build_info_update)
{ MENU_SET_VALUE("%s", SMOOTHAF_BUILD_ID); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_update)
{ MENU_SET_VALUE("%s", smoothaf_algorithm_name()); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_phase_update)
{ MENU_SET_VALUE("%s", smoothaf_algorithm_phase_name()); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_direction_step_update)
{ MENU_SET_VALUE("%d / %d", (int)smoothaf_search_direction, smoothaf_test_step); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_best_score_update)
{
    if (smoothaf_test_session)
        MENU_SET_VALUE("%d", (int)smoothaf_last_best);
    else
        MENU_SET_VALUE("%d", (int)smoothaf_vlog_af.best);
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_best_position_update)
{
    if (smoothaf_test_session)
        MENU_SET_VALUE("%d", (int)smoothaf_last_algorithm_focus_pos);
    else
        MENU_SET_VALUE("%d", (int)smoothaf_vlog_af.best_position);
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_search_evolution_update)
{
    if (smoothaf_test_session)
        MENU_SET_VALUE("cmd=%d", (int)smoothaf_last_commands);
    else
        MENU_SET_VALUE("cmd=%d", (int)smoothaf_vlog_af.algorithm_commands);
}

static MENU_UPDATE_FUNC(smoothaf_test_commands_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_last_commands);
}

static MENU_UPDATE_FUNC(smoothaf_test_best_score_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_last_best);
}

static MENU_UPDATE_FUNC(smoothaf_test_best_position_update)
{
    MENU_SET_VALUE("%d", (int)smoothaf_last_algorithm_focus_pos);
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_movement_update)
{ MENU_SET_VALUE("%s", smoothaf_movement_state_name()); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_reversals_update)
{
    int reversals = smoothaf_test_session
                   ? (int)smoothaf_last_reversals
                   : (int)smoothaf_vlog_af.reversals;
    MENU_SET_VALUE("%d", reversals);
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_convergence_update)
{ MENU_SET_VALUE("%s", smoothaf_vlog_af_result_name(smoothaf_af_episode_result)); }

static MENU_UPDATE_FUNC(smoothaf_algorithm_evaluation_update)
{
    MENU_SET_VALUE("%s", smoothaf_physical_movement_state == SMOOTHAF_MOVEMENT_MOVING
                   ? "Moving" : "Idle");
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_last_result_update)
{
    MENU_SET_VALUE("%s", smoothaf_test_session && smoothaf_last_algorithm_valid
                   ? smoothaf_status_name_for(smoothaf_last_algorithm_status)
                   : smoothaf_vlog_af_result_name(smoothaf_vlog_af.episode_result));
}

static MENU_UPDATE_FUNC(smoothaf_algorithm_time_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_last_time); }

static MENU_UPDATE_FUNC(smoothaf_test_auto_reversals_update)
{ MENU_SET_VALUE("%d / %d", (int)smoothaf_test_auto_reversals, SMOOTHAF_AUTO_MAX_REVERSALS); }

static const char *smoothaf_test_auto_result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_AUTO_FIRST_IMPROVED: return "FIRST IMPROVED";
        case SMOOTHAF_AUTO_OPPOSITE_IMPROVED: return "OPPOSITE IMPROVED";
        case SMOOTHAF_AUTO_UNCERTAIN: return "UNCERTAIN";
        case SMOOTHAF_AUTO_LOW_CONFIDENCE: return "LOW CONFIDENCE";
        case SMOOTHAF_AUTO_INVALID: return "INVALID";
        case SMOOTHAF_AUTO_ABORTED: return "ABORTED";
        case SMOOTHAF_AUTO_LIMIT: return "LIMIT";
        default: return "N/A";
    }
}

static MENU_UPDATE_FUNC(smoothaf_test_auto_result_update)
{ MENU_SET_VALUE("%s", smoothaf_test_auto_result_name(smoothaf_test_auto_result)); }

static MENU_UPDATE_FUNC(smoothaf_test_auto_commands_update)
{ MENU_SET_VALUE("%d / %d", (int)smoothaf_test_auto_commands, SMOOTHAF_AUTO_MAX_COMMANDS); }

static MENU_UPDATE_FUNC(smoothaf_test_auto_scores_update)
{
    MENU_SET_VALUE("%d -> %d", (int)smoothaf_test_auto_first_score,
                   (int)smoothaf_test_auto_opposite_score);
}

static MENU_UPDATE_FUNC(smoothaf_test_auto_deltas_update)
{
    MENU_SET_VALUE("%d / %d", (int)smoothaf_test_auto_first_delta,
                   (int)smoothaf_test_auto_opposite_delta);
}

static MENU_UPDATE_FUNC(smoothaf_monitor_confidence_update)
{ MENU_SET_VALUE("valid=%d low=%d", (int)smoothaf_monitor.metrics.valid,
                  (int)smoothaf_monitor.metrics.low_confidence); }

static MENU_SELECT_FUNC(smoothaf_stop_select)
{
    int monitor_was_enabled = smoothaf_monitor.enabled;
    if (smoothaf_lock())
    {
        smoothaf_cancel = 1;
        smoothaf_task_stop_requested = 1;
        if (!smoothaf_task_running) smoothaf_status = SMOOTHAF_STATUS_IDLE;
        smoothaf_unlock();
    }
    smoothaf_request_episode_stop();
    if (smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_NONE ||
        smoothaf_af_episode_result == SMOOTHAF_VLOG_AF_RESULT_ARMED)
        smoothaf_af_episode_result = smoothaf_vlog_af.episode_result;
    smoothaf_cancel_pending_measure();
    smoothaf_movie_automatic_session = 0;
    smoothaf_movie_start_pending = 0;
    if (monitor_was_enabled)
    {
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_log_transitions();
    }
}

/* Emergency stop does not acquire smoothaf_sem: it remains usable while a
 * focus task owns the semaphore or is blocked in lens_focus(). */
static MENU_SELECT_FUNC(smoothaf_panic_select)
{
    smoothaf_panic_latch(&smoothaf_enabled, &smoothaf_cancel, &smoothaf_panic);
    smoothaf_task_stop_requested = 1;
    smoothaf_status = SMOOTHAF_STATUS_CANCELLED;
    if (!smoothaf_monitor_task_requested && !smoothaf_task_running)
        smoothaf_episode_stop();
    smoothaf_cancel_pending_measure();
    smoothaf_monitor_stop(&smoothaf_monitor);
    smoothaf_movie_automatic_session = 0;
    smoothaf_movie_start_pending = 0;
}

static MENU_SELECT_FUNC(smoothaf_rearm_select)
{
    if (smoothaf_task_running || !smoothaf_task_exited)
        return;
    smoothaf_panic_rearm(&smoothaf_cancel, &smoothaf_panic);
    smoothaf_enabled = 0;
    smoothaf_status = SMOOTHAF_STATUS_IDLE;
}

static MENU_UPDATE_FUNC(smoothaf_status_update)
{
    MENU_SET_VALUE("%s", smoothaf_status_name());
    if (smoothaf_last_block)
    {
        MENU_SET_HELP("score=%d mean=%d t=%dms pos=%d blk=%s call=%d ret=%d",
                      (int)smoothaf_last_score, (int)smoothaf_last_mean,
                      (int)smoothaf_last_time, (int)smoothaf_last_focus_pos,
                      smoothaf_block_name(smoothaf_last_block),
                      (int)smoothaf_last_lens_called,
                      (int)smoothaf_last_lens_return);
    }
    else
    {
        MENU_SET_HELP("score=%d best=%d mean=%d t=%dms pos=%d cmds=%d rev=%d",
                      (int)smoothaf_last_score, (int)smoothaf_last_best,
                      (int)smoothaf_last_mean, (int)smoothaf_last_time,
                      (int)smoothaf_last_focus_pos,
                      (int)smoothaf_last_commands,
                      (int)smoothaf_last_reversals);
    }
}

static struct menu_entry smoothaf_menu[] =
{
    { .name = "Enable", .priv = &smoothaf_enabled, .max = 1,
      .help = "Enable lens movement for explicit focus actions." },
    { .name = "Vlog Monitor", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Monitoring", .select = smoothaf_monitor_toggle_select,
          .update = smoothaf_monitor_toggle_update,
          .help = "Toggle periodic read-only monitoring; no automatic lens movement." },
        { .name = "Start New AF Episode", .select = smoothaf_vlog_af_episode_select,
          .help = "Manual LiveView arm; Movie H.264 REC START arms continuous mode automatically." },
        { .name = "Vlog AF Mode", .priv = (void *)&smoothaf_vlog_af_mode,
          .min = SMOOTHAF_VLOG_AF_OFF, .max = SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS,
          .choices = CHOICES("OFF", "DRY RUN", "ACTIVE", "ACTIVE CONTINUOUS"),
          .update = smoothaf_vlog_af_mode_update,
          .help = "Selected mode is independent from monitor/episode authorization." },
        { .name = "AF Algorithm", .priv = &smoothaf_vlog_algorithm,
          .min = SMOOTHAF_ALGORITHM_CURRENT_VLOG,
          .max = SMOOTHAF_ALGORITHM_NORMAL_AF,
          .choices = CHOICES("Current Vlog", "Legacy HC", "Adaptive HC", "Normal AF"),
          .update = smoothaf_vlog_algorithm_update,
          .help = "Selects the real Vlog episode adapter; changes apply at a safe episode boundary." },
        { .name = "AF Direction", .priv = &smoothaf_vlog_direction,
          .min = SMOOTHAF_VLOG_DIRECTION_AUTO, .max = SMOOTHAF_VLOG_DIRECTION_B,
          .choices = CHOICES("AUTO", "A", "B"),
          .help = "AUTO probes both sides once; A/B force the initial direction. Focus Test is independent." },
        { .name = "Movie Active Exp", .priv = &smoothaf_movie_active_experimental,
          .max = 1,
          .help = "Authorize physical lens_focus only for normal H.264 Movie; default OFF." },
        { .name = "Far Focus Recovery", .priv = &smoothaf_far_focus_recovery,
          .max = 1, .update = smoothaf_vlog_recovery_update,
          .help = "Experimental bounded coarse/bracket/fine recovery; physical movement is Movie-gated." },
        { .name = "Vlog Metric", .priv = &smoothaf_vlog_metric,
          .min = SMOOTHAF_METRIC_SAD, .max = SMOOTHAF_METRIC_LAPLACIAN,
          .choices = CHOICES("SAD", "Laplacian"),
          .help = "Sharpness metric used by Vlog monitoring and AF." },
        { .name = "Vlog Step", .priv = &smoothaf_vlog_step, .min = 1, .max = 3,
          .unit = UNIT_DEC, .help = "Physical step used by Vlog AF; default is 3." },
        { .name = "Sample Int", .priv = &smoothaf_monitor_interval,
          .min = SMOOTHAF_MONITOR_INTERVAL_1000, .max = SMOOTHAF_MONITOR_INTERVAL_100,
          .choices = CHOICES("1000 ms", "500 ms", "250 ms", "100 ms"),
          .help = "Monitor sampling interval; the same setting drives the monitor task." },
        { .name = "Session State", .update = smoothaf_vlog_af_session_state_update },
        { .name = "AF State", .update = smoothaf_vlog_af_state_update },
        { .name = "Monitor Status", .update = smoothaf_monitor_status_update,
          .help = "Periodic read-only sharpness monitoring; details appear below when selected." },
        { .name = "EP Result", .update = smoothaf_vlog_af_result_update,
          .help = "Latest bounded episode outcome and reason." },
        MENU_EOL,
    } },
    { .name = "Diagnostic", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Measure", .select = smoothaf_measure_select,
          .help = "Read-only central 64x64 ROI measurement; Enable may remain off." },
        { .name = "Measure Result", .update = smoothaf_measure_result_update,
          .help = "Latest manual measurement; WAITING means queued on the monitor task." },
        { .name = "Focus Step", .select = smoothaf_step_select,
          .help = "Execute one explicitly requested focus step." },
        { .name = "Direction", .priv = &smoothaf_direction_choice, .min = 0,
          .max = 1, .choices = CHOICES("Direction A", "Direction B"),
          .help = "Diagnostic/manual relative motor direction; does not control automatic Vlog AF." },
        { .name = "Debug Logging", .priv = &smoothaf_log.enabled, .min = 0, .max = 1,
          .help = "Record MOVE, MEASURE, CONTROLLER, ERROR and TASK-END events in RAM." },
        { .name = "Persistent Logger", .priv = &smoothaf_logger_enabled, .min = 0, .max = 1,
          .select = smoothaf_persist_logger_config_select,
          .help = "Enable RAM logging of AF episodes (session/episode/command/measurement/timeout)." },
        { .name = "Logger FIO Enable", .priv = &smoothaf_logger_fio_enabled, .min = 0, .max = 1,
          .select = smoothaf_persist_logger_config_select,
          .help = "Enable file I/O backend for persistent SD logging (requires Persistent Logger ON)." },
        { .name = "Logger Max File KB", .priv = &smoothaf_logger_max_file_kb, .min = 64, .max = 1024,
          .unit = UNIT_DEC, .help = "Maximum KB per log file before rotation." },
        { .name = "Logger Max Files", .priv = &smoothaf_logger_max_files, .min = 1, .max = 9,
          .help = "Maximum number of rotated log files (SMTH000.LOG..SMTH008.LOG)." },
        { .name = "Logger Fill Level", .update = smoothaf_logger_fill_update,
          .help = "Current RAM ring buffer fill (0..255)." },
        { .name = "Logger Dropped", .update = smoothaf_logger_dropped_update,
          .help = "Records lost due to full buffer." },
        { .name = "Logger FIO Errors", .update = smoothaf_logger_fio_errors_update,
          .help = "FIO write errors (card unavailable, partial write, file full)." },
        { .name = "Logger FIO Partials", .update = smoothaf_logger_fio_partials_update,
          .help = "Partial FIO writes (buffer full during write)." },
        { .name = "Logger FIO Files", .update = smoothaf_logger_fio_files_update,
          .help = "Times file rotation triggered (size limit reached)." },
        { .name = "Log Page", .update = smoothaf_diag_page_update,
          .help = "EP/event page in the fixed RAM snapshot." },
        { .name = "Log Entry", .update = smoothaf_diag_entry_update,
          .help = "Read-only record; navigation never moves the lens." },
        { .name = "Previous", .select = smoothaf_diag_previous_select,
          .help = "Show the previous diagnostic record." },
        { .name = "Next", .select = smoothaf_diag_next_select,
          .help = "Show the next diagnostic record." },
        { .name = "Clear History", .select = smoothaf_diag_clear_select,
          .help = "Clear records and page tracking." },
        { .name = "Dropped", .update = smoothaf_diag_dropped_update,
          .help = "Events overwritten after the bounded RAM ring filled." },
        { .name = "EP Summary", .update = smoothaf_episode_summary_update,
          .help = "Newest preserved bounded episode summary; no physical confirmation is implied." },
        { .name = "Previous EP", .select = smoothaf_episode_summary_previous_select },
        { .name = "Next EP", .select = smoothaf_episode_summary_next_select },
        MENU_EOL,
    } },
    { .name = "AF Performance", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "AF Session", .update = smoothaf_vlog_af_session_state_update },
        { .name = "AF State", .update = smoothaf_vlog_af_state_update },
        { .name = "EP Result", .update = smoothaf_vlog_af_result_update },
        { .name = "EP Reason", .update = smoothaf_af_episode_reason_update },
        { .name = "AF EPs Done", .update = smoothaf_af_episodes_completed_update },
        { .name = "Move Attempts", .update = smoothaf_af_move_attempts_update },
        { .name = "Lens Calls", .update = smoothaf_af_lens_calls_update },
        { .name = "Move Success", .update = smoothaf_af_move_successes_update },
        { .name = "AF CMDs This EP", .update = smoothaf_af_commands_episode_update },
        { .name = "AUTO Probe CMDs", .update = smoothaf_af_auto_probe_commands_update },
        { .name = "Algorithm CMDs", .update = smoothaf_af_algorithm_commands_update },
        { .name = "AF CMD Limit", .priv = &smoothaf_vlog_episode_limit,
          .min = SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT, .max = SMOOTHAF_VLOG_AF_MAX_COMMANDS },
        { .name = "AF CMDs Left", .update = smoothaf_vlog_af_available_update },
        { .name = "Initial Score", .update = smoothaf_af_initial_score_update },
        { .name = "Post Score", .update = smoothaf_af_post_move_score_update },
        { .name = "Best Score", .update = smoothaf_af_best_score_update },
        { .name = "Score Delta", .update = smoothaf_af_score_delta_update },
        { .name = "Post Valid", .update = smoothaf_af_post_move_valid_update },
        { .name = "Result Valid", .update = smoothaf_af_last_result_valid_update },
        { .name = "Current Pos", .update = smoothaf_af_current_position_update },
        { .name = "Best Pos", .update = smoothaf_af_best_position_update },
        { .name = "AF Fail Streak", .update = smoothaf_af_consecutive_failures_update },
        { .name = "AF CD Remain", .update = smoothaf_af_cooldown_update },
        { .name = "AF Since Move", .update = smoothaf_vlog_af_last_command_update },
        { .name = "Next EP Block", .update = smoothaf_af_next_block_update },
        { .name = "Blur Recheck", .update = smoothaf_af_recheck_update },
        { .name = "AF Armed", .update = smoothaf_af_continuous_update },
        { .name = "AF Last Dir", .update = smoothaf_af_last_direction_update },
        { .name = "Phys Move", .update = smoothaf_af_movement_update },
        { .name = "CMD Block", .update = smoothaf_af_command_block_update },
        { .name = "Last Block", .update = smoothaf_af_last_block_update },
        { .name = "Before Error", .update = smoothaf_af_episode_state_before_error_update },
        { .name = "Timeout Remain", .update = smoothaf_vlog_af_timeout_update },
        { .name = "Last Move Result", .update = smoothaf_af_last_move_result_update },
        MENU_EOL,
    } },
    { .name = "Monitor Performance", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Monitor Status", .update = smoothaf_monitor_status_update },
        { .name = "SAD Score", .update = smoothaf_monitor_sad_update },
        { .name = "Lap Score", .update = smoothaf_monitor_laplacian_update },
        { .name = "Smooth Score", .update = smoothaf_monitor_smoothed_update },
        { .name = "Ref Score", .update = smoothaf_monitor_reference_update },
        { .name = "Possible Blur", .update = smoothaf_monitor_blur_update },
        { .name = "Conf/Valid", .update = smoothaf_monitor_confidence_update },
        { .name = "Samples", .update = smoothaf_monitor_samples_update },
        { .name = "REC State", .update = smoothaf_movie_recording_update },
        { .name = "Movie Dry Run", .update = smoothaf_movie_recording_update },
        { .name = "Task State", .update = smoothaf_monitor_task_state_update },
        { .name = "Actual Int", .update = smoothaf_monitor_actual_interval_update },
        { .name = "Proc Dur", .update = smoothaf_monitor_last_duration_update },
        { .name = "Avg Dur", .update = smoothaf_monitor_average_duration_update },
        { .name = "Max Dur", .update = smoothaf_monitor_max_duration_update },
        { .name = "Valid Samples", .update = smoothaf_monitor_valid_update },
        { .name = "Invalid Samples", .update = smoothaf_monitor_invalid_update },
        { .name = "Low Conf", .update = smoothaf_monitor_low_confidence_update },
        { .name = "Blocked", .update = smoothaf_monitor_blocked_update },
        { .name = "Skipped", .update = smoothaf_monitor_skipped_update },
        { .name = "Overruns", .update = smoothaf_monitor_overruns_update },
        { .name = "Last Error", .update = smoothaf_monitor_error_update },
        MENU_EOL,
    } },
    { .name = "Algorithm Performance", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Algorithm", .update = smoothaf_algorithm_update },
        { .name = "Search Phase", .update = smoothaf_algorithm_phase_update },
        { .name = "Search Dir", .update = smoothaf_algorithm_direction_step_update },
        { .name = "Search Step", .update = smoothaf_algorithm_direction_step_update },
        { .name = "Best Score", .update = smoothaf_algorithm_best_score_update },
        { .name = "Best Pos", .update = smoothaf_algorithm_best_position_update },
        { .name = "Search Evol", .update = smoothaf_algorithm_search_evolution_update },
        { .name = "Reversals", .update = smoothaf_algorithm_reversals_update },
        { .name = "Converge Why", .update = smoothaf_algorithm_convergence_update },
        { .name = "Algo Eval", .update = smoothaf_algorithm_evaluation_update },
        { .name = "Eval Time", .update = smoothaf_algorithm_time_update },
        { .name = "Last Algo Result", .update = smoothaf_algorithm_last_result_update },
        { .name = "CMD Pos", .update = smoothaf_af_current_position_update },
        { .name = "Lens Pos", .update = smoothaf_af_current_position_update },
        MENU_EOL,
    } },
    { .name = "Focus Test", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Start Focus Test", .select = smoothaf_search_select,
          .help = "Run the selected bounded controller; LiveView only." },
        { .name = "AF Algorithm", .priv = &smoothaf_af_mode,
          .min = SMOOTHAF_HC_LEGACY, .max = SMOOTHAF_AF_NORMAL,
          .choices = CHOICES("Legacy HC", "Adaptive HC", "Normal AF"),
          .help = "Manual Focus Test controller; Vlog AF is independent." },
        { .name = "Test Metric", .priv = &smoothaf_test_metric,
          .min = SMOOTHAF_METRIC_SAD, .max = SMOOTHAF_METRIC_LAPLACIAN,
          .choices = CHOICES("SAD", "Laplacian"),
          .help = "Sharpness metric used only by Focus Test." },
        { .name = "Test Step", .priv = &smoothaf_test_step, .min = 1, .max = 3,
          .unit = UNIT_DEC, .help = "Physical step used only by Focus Test; default is 3." },
        { .name = "Test Direction", .priv = &smoothaf_test_direction,
          .min = SMOOTHAF_TEST_DIRECTION_AUTO, .max = SMOOTHAF_TEST_DIRECTION_B,
          .choices = CHOICES("AUTO", "A", "B"),
          .help = "AUTO probes both sides; A/B set the initial direction." },
        { .name = "Test Status", .update = smoothaf_status_update },
        { .name = "Test Result", .update = smoothaf_algorithm_last_result_update },
        { .name = "AUTO Result", .update = smoothaf_test_auto_result_update },
        { .name = "AUTO CMDs", .update = smoothaf_test_auto_commands_update },
        { .name = "AUTO Reversals", .update = smoothaf_test_auto_reversals_update },
        { .name = "AUTO Scores", .update = smoothaf_test_auto_scores_update },
        { .name = "AUTO Deltas", .update = smoothaf_test_auto_deltas_update },
        { .name = "Test Duration", .update = smoothaf_algorithm_time_update },
        { .name = "Test CMDs", .update = smoothaf_test_commands_update },
        { .name = "Best Score", .update = smoothaf_test_best_score_update },
        { .name = "Best Pos", .update = smoothaf_test_best_position_update },
        MENU_EOL,
    } },
    { .name = "Status", .update = smoothaf_status_update },
    { .name = "STOP", .select = smoothaf_stop_select,
      .help = "Cancel future movement; a synchronous lens command may finish first." },
    { .name = "PANIC / Emergency Stop", .select = smoothaf_panic_select,
      .help = "Latch cancellation and disable focus; does not interrupt lens_focus()." },
    { .name = "Rearm Safe Mode", .select = smoothaf_rearm_select,
      .help = "Clear PANIC only after the previous task has ended; Enable remains off." },
    { .name = "Build Info", .update = smoothaf_build_info_update,
      .help = "Deterministic source/configuration identifier for this module." },
};

static unsigned int smoothaf_init(void)
{
    smoothaf_movie_active_experimental = 0;
    smoothaf_sem = create_named_semaphore("smoothaf_sem", 1);
    if (!smoothaf_sem) return 1;
    smoothaf_enabled = 0;
    smoothaf_panic = 0;
    smoothaf_cancel = 0;
    smoothaf_task_running = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 1;
    smoothaf_status = SMOOTHAF_STATUS_IDLE;
    smoothaf_last_algorithm_valid = 0;
    smoothaf_last_algorithm_status = SMOOTHAF_STATUS_IDLE;
    smoothaf_test_session = 0;
    smoothaf_test_auto_result = SMOOTHAF_AUTO_INVALID;
    smoothaf_test_auto_commands = 0;
    smoothaf_test_auto_reversals = 0;
    smoothaf_test_auto_first_score = 0;
    smoothaf_test_auto_opposite_score = 0;
    smoothaf_test_auto_first_delta = 0;
    smoothaf_test_auto_opposite_delta = 0;
    smoothaf_physical_movement_state = SMOOTHAF_MOVEMENT_IDLE;
    smoothaf_last_error = 0;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    smoothaf_last_measure_code = SMOOTHAF_MEASURE_WAITING;
    smoothaf_af_move_attempts = 0;
    smoothaf_af_lens_calls = 0;
    smoothaf_af_move_successes = 0;
    smoothaf_af_commands_episode = 0;
    smoothaf_af_initial_score = 0;
    smoothaf_af_post_move_score = 0;
    smoothaf_af_best_score = 0;
    smoothaf_af_score_delta = 0;
    smoothaf_af_best_position = 0;
    smoothaf_af_current_position = 0;
    smoothaf_af_last_direction = 1;
    smoothaf_af_confidence = 0;
    smoothaf_af_decision_relation = 0;
    smoothaf_af_episode_id = 0;
    smoothaf_af_episode_complete = 0;
    smoothaf_af_episode_summary_pending = 0;
    smoothaf_af_episode_block_reason = 0;
    smoothaf_af_episode_cancel_reason = 0;
    smoothaf_af_episode_lens_return = 0;
    smoothaf_measure_requested = 0;
    smoothaf_persist_session_active = 0;
    smoothaf_persist_session_episodes = 0;
    smoothaf_fio_shutdown_requested = 0;
    smoothaf_fio_task_created = 0;
    smoothaf_fio_task_running = 0;
    smoothaf_fio_task_exited = 1;
    smoothaf_fio_seen_errors = 0;
    smoothaf_fio_seen_partials = 0;
    smoothaf_episode_log_init(&smoothaf_episode_log);
    smoothaf_episode_summary_count = 0;
    smoothaf_episode_summary_page = 0;
    smoothaf_episode_summary_seen_count = 0;
    smoothaf_af_episode_complete = 0;
    smoothaf_af_episode_summary_pending = 0;
    smoothaf_af_episode_block_reason = 0;
    smoothaf_af_episode_cancel_reason = 0;
    smoothaf_af_episode_lens_return = 0;
    smoothaf_monitor_init(&smoothaf_monitor, smoothaf_vlog_metric,
                          SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
    smoothaf_movie_recording_init(&smoothaf_movie_recording);
    smoothaf_monitor_task_requested = 0;
    smoothaf_monitor_deinit = 0;
    smoothaf_fio_shutdown_requested = 0;
    smoothaf_fio_task_created = 0;
    smoothaf_fio_task_running = 0;
    smoothaf_fio_task_exited = 1;
    smoothaf_fio_seen_errors = 0;
    smoothaf_fio_seen_partials = 0;
    smoothaf_vlog_af_init(&smoothaf_vlog_af);
    smoothaf_vlog_af_mode = SMOOTHAF_VLOG_AF_OFF;
    smoothaf_vlog_algorithm = smoothaf_algorithm_normalize(smoothaf_vlog_algorithm);
    smoothaf_vlog_algorithm_pending = smoothaf_vlog_algorithm;
    smoothaf_vlog_algorithm_active = smoothaf_vlog_algorithm;
    smoothaf_vlog_algorithm_pending_flag = 0;
    smoothaf_vlog_algorithm_episode = smoothaf_vlog_algorithm;
    smoothaf_far_focus_recovery = smoothaf_far_focus_recovery ? 1 : 0;
    smoothaf_movie_automatic_session = 0;
    smoothaf_movie_start_pending = 0;
    smoothaf_af_episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    smoothaf_log_init(&smoothaf_log);
    smoothaf_diag_count = 0;

    /* Persistent Logger (Phase 3A) - initialize but keep disabled by default */
    smoothaf_persist_logger_init(&smoothaf_persist_log);
    smoothaf_persist_logger_set_enabled(&smoothaf_persist_log, smoothaf_logger_enabled ? true : false);
    smoothaf_log_fio_init(&smoothaf_persist_fio);
    smoothaf_log_fio_request_enabled(&smoothaf_persist_fio,
                                     (smoothaf_logger_fio_enabled && smoothaf_logger_enabled) ? true : false);
    if (smoothaf_logger_fio_enabled && smoothaf_logger_enabled &&
        !smoothaf_start_fio_task()) {
        smoothaf_logger_fio_enabled = 0;
        smoothaf_last_error = 1;
        smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
    }
    if (smoothaf_logger_max_file_kb > 0) {
        smoothaf_persist_fio.max_file_size_bytes = (uint32_t)smoothaf_logger_max_file_kb * 1024;
    }
    if (smoothaf_logger_max_files > 0 && smoothaf_logger_max_files <= SMOOTHAF_LOG_FIO_MAX_FILES) {
        smoothaf_persist_fio.max_files = (uint32_t)smoothaf_logger_max_files;
    }
    smoothaf_diag_page = 0;
    smoothaf_diag_seen_total = 0;
    smoothaf_telemetry_init(&smoothaf_telemetry);
    if (smoothaf_metric < SMOOTHAF_METRIC_SAD ||
        smoothaf_metric > SMOOTHAF_METRIC_LAPLACIAN) smoothaf_metric = SMOOTHAF_METRIC_SAD;
    if (smoothaf_test_metric < SMOOTHAF_METRIC_SAD ||
        smoothaf_test_metric > SMOOTHAF_METRIC_LAPLACIAN) smoothaf_test_metric = smoothaf_metric;
    if (smoothaf_vlog_metric < SMOOTHAF_METRIC_SAD ||
        smoothaf_vlog_metric > SMOOTHAF_METRIC_LAPLACIAN) smoothaf_vlog_metric = smoothaf_metric;
    smoothaf_metric = smoothaf_vlog_metric;
    if (smoothaf_step < 1 || smoothaf_step > 3) smoothaf_step = 3;
    if (smoothaf_test_step < 1 || smoothaf_test_step > 3)
        smoothaf_test_step = config_var_was_changed(&smoothaf_step)
                           ? smoothaf_step : 3;
    if (smoothaf_vlog_step < 1 || smoothaf_vlog_step > 3)
        smoothaf_vlog_step = config_var_was_changed(&smoothaf_step)
                          ? smoothaf_step : 3;
    if (smoothaf_test_direction < SMOOTHAF_TEST_DIRECTION_AUTO ||
        smoothaf_test_direction > SMOOTHAF_TEST_DIRECTION_B)
        smoothaf_test_direction = SMOOTHAF_TEST_DIRECTION_AUTO;
    if (smoothaf_direction_choice < 0 || smoothaf_direction_choice > 1)
        smoothaf_direction_choice = 0;
    if (smoothaf_af_direction_choice < 0 || smoothaf_af_direction_choice > 1)
        smoothaf_af_direction_choice = 0;
    smoothaf_vlog_direction = smoothaf_vlog_direction_migrate(
        smoothaf_vlog_direction,
        config_var_was_changed(&smoothaf_vlog_direction),
        smoothaf_af_direction_choice,
        config_var_was_changed(&smoothaf_af_direction_choice));
    if (smoothaf_hc_mode < SMOOTHAF_HC_LEGACY ||
        smoothaf_hc_mode > SMOOTHAF_HC_ADAPTIVE) smoothaf_hc_mode = SMOOTHAF_HC_LEGACY;
    if (smoothaf_af_mode < SMOOTHAF_HC_LEGACY ||
        smoothaf_af_mode > SMOOTHAF_AF_NORMAL)
        smoothaf_af_mode = smoothaf_hc_mode == SMOOTHAF_HC_ADAPTIVE
                         ? SMOOTHAF_HC_ADAPTIVE : SMOOTHAF_HC_LEGACY;
    smoothaf_monitor_interval = smoothaf_monitor_normalize_interval(smoothaf_monitor_interval);
    smoothaf_monitor.interval_index = smoothaf_monitor_interval;
    menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));
    return 0;
}

static unsigned int smoothaf_deinit(void)
{
    int i;
    smoothaf_monitor_deinit = 1;
    smoothaf_task_stop_requested = 1;
    smoothaf_fio_shutdown_requested = 0;
    smoothaf_cancel_pending_measure();
    smoothaf_monitor_stop(&smoothaf_monitor);
    if (smoothaf_lock())
    {
        smoothaf_enabled = 0;
        smoothaf_cancel = 1;
        smoothaf_unlock();
    }
    else
    {
        smoothaf_enabled = 0;
        smoothaf_cancel = 1;
    }
    for (i = 0; i < SMOOTHAF_DEINIT_WAIT_MS / 10 &&
         (smoothaf_is_task_running() || !smoothaf_task_exited); i++) msleep(10);
    if (!smoothaf_is_task_running() && smoothaf_af_episode_id &&
        !smoothaf_af_episode_complete)
        smoothaf_episode_stop();
    if (smoothaf_is_task_running() || !smoothaf_task_exited)
    {
        smoothaf_last_error = 1;
        smoothaf_last_block = SMOOTHAF_BLOCK_CANCELLED;
    }

    /* Stop the independent consumer without waiting for card I/O. The task
     * owns FILE * and will close it as part of its own exit path. If it is
     * already inside a non-cancellable FIO/lens primitive, leave the menu in
     * place and report the unload hazard instead of blocking deinit. */
    smoothaf_fio_shutdown_requested = 1;
    smoothaf_log_fio_request_close(&smoothaf_persist_fio);
    smoothaf_persist_logger_set_enabled(&smoothaf_persist_log, false);
    if (smoothaf_fio_task_created && !smoothaf_fio_task_exited)
    {
        smoothaf_last_error = 1;
        smoothaf_last_block = SMOOTHAF_BLOCK_CANCELLED;
    }

    if (smoothaf_is_task_running() || !smoothaf_task_exited ||
        (smoothaf_fio_task_created && !smoothaf_fio_task_exited))
        return 1;
    menu_remove("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(smoothaf_init)
    MODULE_DEINIT(smoothaf_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, smoothaf_movie_recording_cbr, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(smoothaf_metric)
    MODULE_CONFIG(smoothaf_test_metric)
    MODULE_CONFIG(smoothaf_vlog_metric)
    MODULE_CONFIG(smoothaf_step)
    MODULE_CONFIG(smoothaf_test_step)
    MODULE_CONFIG(smoothaf_vlog_step)
    MODULE_CONFIG(smoothaf_direction_choice)
    MODULE_CONFIG(smoothaf_af_direction_choice)
    MODULE_CONFIG(smoothaf_vlog_direction)
    MODULE_CONFIG(smoothaf_test_direction)
    MODULE_CONFIG(smoothaf_hc_mode)
    MODULE_CONFIG(smoothaf_af_mode)
    MODULE_CONFIG(smoothaf_vlog_algorithm)
    MODULE_CONFIG(smoothaf_vlog_algorithm_pending)
    MODULE_CONFIG(smoothaf_monitor_interval)
    MODULE_CONFIG(smoothaf_vlog_episode_limit)
    MODULE_CONFIG(smoothaf_far_focus_recovery)
    MODULE_CONFIG(smoothaf_movie_active_experimental)
    MODULE_CONFIG(smoothaf_logger_enabled)
    MODULE_CONFIG(smoothaf_logger_fio_enabled)
    MODULE_CONFIG(smoothaf_logger_max_file_kb)
    MODULE_CONFIG(smoothaf_logger_max_files)
MODULE_CONFIGS_END()
