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

#define SMOOTHAF_HC_LEGACY       0
#define SMOOTHAF_HC_ADAPTIVE     1
#define SMOOTHAF_AF_NORMAL       2

/* Experimental timing values for the EF 50mm f/1.8 STM.  They have not been
 * calibrated on hardware; lens_focus(wait=1) itself can block for up to about
 * two seconds (property wait + focus confirmation wait). */
#define SMOOTHAF_SETTLE_MS        120
#define SMOOTHAF_COOLDOWN_MS      80
#define SMOOTHAF_TIMEOUT_MS       5000

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
static struct smoothaf_log_buffer smoothaf_log;
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
static volatile int smoothaf_last_mode = SMOOTHAF_HC_LEGACY;
static volatile int smoothaf_last_phase = 0;

static struct smoothaf_monitor smoothaf_monitor;
static volatile int smoothaf_monitor_task_requested = 0;
static volatile int smoothaf_monitor_deinit = 0;
static volatile int smoothaf_log_publish_failures = 0;
static volatile int smoothaf_last_measure_code = 0;
#define SMOOTHAF_MEASURE_OK       0
#define SMOOTHAF_MEASURE_LOW      1
#define SMOOTHAF_MEASURE_ERROR    2
#define SMOOTHAF_MEASURE_WAITING  3
static struct smoothaf_log_entry smoothaf_diag_snapshot[SMOOTHAF_DIAG_CAPACITY];
static size_t smoothaf_diag_count = 0;
static size_t smoothaf_diag_page = 0;
static size_t smoothaf_diag_seen_count = 0;

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
    smoothaf_unlock();
}

static void smoothaf_log_event(smoothaf_log_type_t type,
                               int32_t value1, int32_t value2, int32_t value3)
{
    smoothaf_log_event_extended(type, value1, value2, value3, 0, 0);
}

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
        return 0;
    }
    if (smoothaf_step < 1 || smoothaf_step > 3)
    {
        smoothaf_last_block = SMOOTHAF_BLOCK_PARAMS;
        smoothaf_last_error = 1;
        return 0;
    }

    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        return 0;
    }

    smoothaf_status = SMOOTHAF_STATUS_MOVING;
    start = get_ms_clock();

    /* wait=1 is intentional: one command at a time, confirmed by the core. */
    smoothaf_last_lens_called = 1;
    smoothaf_log_event(SMOOTHAF_LOG_MOVE, direction, smoothaf_step, 0);
    result = lens_focus(direction, smoothaf_step, 1, 0);
    smoothaf_last_lens_return = result;

    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - (uint32_t)start);
    smoothaf_last_focus_pos = lens_info.focus_pos;

    if (!result)
    {
        reason = smoothaf_block_reason();
        if (reason) smoothaf_last_block = reason;
        smoothaf_last_error = 1;
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
    struct smoothaf_monitor_sample sample;

    while (smoothaf_monitor.enabled && !smoothaf_cancel &&
           !smoothaf_monitor_deinit && !smoothaf_task_stop_requested)
    {
        if (smoothaf_monitor_due((uint32_t)get_ms_clock(),
                                 smoothaf_monitor.metrics.next_due_ms))
        {
            if (smoothaf_measure_block_reason())
            {
                smoothaf_monitor_record_blocked(&smoothaf_monitor);
                smoothaf_monitor_transition(&smoothaf_monitor,
                                            SMOOTHAF_MONITOR_PAUSED);
                smoothaf_monitor_log_transitions();
                msleep(20);
                continue;
            }

            start = (uint32_t)get_ms_clock();
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
            smoothaf_monitor_accept(&smoothaf_monitor, &sample, completed);
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
            smoothaf_last_block = SMOOTHAF_BLOCK_TASK_CREATE;
            smoothaf_last_error = 1;
            smoothaf_status = SMOOTHAF_STATUS_ERROR;
            smoothaf_unlock();
        }
        else
        {
            /* The reservation must not survive a task-creation failure. */
            smoothaf_task_running = 0;
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
                .help = "Toggle passive viewing only; no periodic task or lens movement.",
            },
            {
                .name = "Monitor Status",
                .update = smoothaf_monitor_status_update,
                .help = "Passive state from existing measurements; no automatic sampling.",
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
                .help = "Valid manual samples in the current monitoring session.",
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
    smoothaf_log_init(&smoothaf_log);
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
