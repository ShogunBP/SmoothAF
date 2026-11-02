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
#include "smoothaf_diagnostics.h"

#define SMOOTHAF_HC_LEGACY       0
#define SMOOTHAF_HC_ADAPTIVE     1
#define SMOOTHAF_AF_NORMAL       2

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

static CONFIG_INT("smoothaf.metric", smoothaf_metric, SMOOTHAF_METRIC_SAD);
static CONFIG_INT("smoothaf.step", smoothaf_step, 1);
static CONFIG_INT("smoothaf.dir", smoothaf_direction_choice, 0);
static CONFIG_INT("smoothaf.hc_mode", smoothaf_hc_mode, SMOOTHAF_HC_LEGACY);
static CONFIG_INT("smoothaf.af_mode", smoothaf_af_mode, -1);
static CONFIG_INT("smoothaf.monitor_interval", smoothaf_monitor_interval,
                 SMOOTHAF_MONITOR_INTERVAL_DEFAULT);

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
static volatile int smoothaf_last_error;
static volatile int smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
static volatile int smoothaf_last_commands;
static volatile int smoothaf_last_reversals;
static volatile int smoothaf_last_lens_called;
static volatile int smoothaf_last_lens_return;
static volatile int smoothaf_last_measure_code;
static volatile int smoothaf_last_mode = SMOOTHAF_HC_LEGACY;
static volatile int smoothaf_last_phase;
static uint32_t smoothaf_session_started_ms;
static uint32_t smoothaf_session_finished_ms;
static struct smoothaf_telemetry smoothaf_telemetry;
static struct smoothaf_monitor smoothaf_monitor;
static volatile int smoothaf_monitor_task_requested;
static volatile int smoothaf_monitor_deinit;
static struct semaphore *smoothaf_sem;

#define SMOOTHAF_MEASURE_OK       0
#define SMOOTHAF_MEASURE_LOW      1
#define SMOOTHAF_MEASURE_ERROR    2
#define SMOOTHAF_MEASURE_WAITING  3

static int smoothaf_is_task_running(void);

static const char *smoothaf_status_name(void)
{
    switch (smoothaf_status)
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

static int smoothaf_direction_value(void)
{
    return smoothaf_direction_choice ? 1 : -1;
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
    sample.metric = smoothaf_metric == SMOOTHAF_METRIC_LAPLACIAN
                  ? SMOOTHAF_MONITOR_METRIC_LAPLACIAN
                  : SMOOTHAF_MONITOR_METRIC_SAD;
    sample.duration_ms = duration;
    smoothaf_monitor_accept(&smoothaf_monitor, &sample, (uint32_t)get_ms_clock());
    if (smoothaf_monitor_take_transition(&smoothaf_monitor, &from, &to))
        smoothaf_telemetry_update(score, (int)duration, 0, smoothaf_step,
                                  to, valid ? 0 : 1, valid ? 0 : SMOOTHAF_BLOCK_FRAME);
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
    smoothaf_last_score = valid ? score : -1;
    if (valid) smoothaf_last_best = score;
    smoothaf_monitor_update_from_measurement(valid ? score : -1, mean, sad,
                                             laplacian, valid, 0);
    smoothaf_telemetry_update(valid ? score : -1, 0, 0, smoothaf_step,
                              smoothaf_status, valid ? 0 : 1,
                              valid ? 0 : SMOOTHAF_BLOCK_FRAME);
}

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
    if (is_follow_focus_active()) return SMOOTHAF_BLOCK_FOLLOW_FOCUS;
    if (is_rack_focus_enabled()) return SMOOTHAF_BLOCK_RACK_FOCUS;
    if (is_focus_stack_enabled()) return SMOOTHAF_BLOCK_FOCUS_STACK;
    if (smoothaf_step < 1 || smoothaf_step > 3) return SMOOTHAF_BLOCK_PARAMS;
    return SMOOTHAF_BLOCK_NONE;
}

static int smoothaf_measure(int *score_out, int *mean_out, int *sad_out,
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
    if (!lv || is_movie_mode() || RECORDING_H264 || RECORDING_CUSTOM || RECORDING_RAW)
        return 0;
    vram = get_yuv422_vram();
    if (!vram || !vram->vram ||
        !smoothaf_geometry_ok(vram->width, vram->height, vram->pitch)) return 0;
    left = smoothaf_roi_left(vram->width);
    top = smoothaf_roi_top(vram->height);
    base = vram->vram + top * vram->pitch + left * 2;
    smoothaf_measure_all(base, vram->pitch, &sad, &laplacian, &mean);
    if (score_out) *score_out = smoothaf_metric == SMOOTHAF_METRIC_LAPLACIAN
                               ? laplacian : sad;
    if (mean_out) *mean_out = mean;
    if (sad_out) *sad_out = sad;
    if (laplacian_out) *laplacian_out = laplacian;
    return 1;
}

static int smoothaf_move(int direction)
{
    int start;
    int result;
    int reason;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    if (direction != -1 && direction != 1) reason = SMOOTHAF_BLOCK_PARAMS;
    else if (smoothaf_step < 1 || smoothaf_step > 3) reason = SMOOTHAF_BLOCK_PARAMS;
    else reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_telemetry_update(smoothaf_last_score, 0, direction,
                                  smoothaf_step, smoothaf_status, 1, reason);
        return 0;
    }
    smoothaf_status = SMOOTHAF_STATUS_MOVING;
    start = get_ms_clock();
    smoothaf_last_lens_called = 1;
    smoothaf_telemetry_update(smoothaf_last_score, 0, direction, smoothaf_step,
                              smoothaf_status, 0, SMOOTHAF_BLOCK_NONE);
    result = lens_focus(direction, smoothaf_step, 1, 0);
    smoothaf_last_lens_return = result;
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - (uint32_t)start);
    smoothaf_last_focus_pos = lens_info.focus_pos;
    smoothaf_telemetry_update(smoothaf_last_score, smoothaf_last_time, direction,
                              smoothaf_step, smoothaf_status, result ? 0 : 1,
                              result ? 0 : SMOOTHAF_BLOCK_NONE);
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
                smoothaf_monitor_record_skip(&smoothaf_monitor);
                smoothaf_monitor_schedule_after(&smoothaf_monitor,
                                                (uint32_t)get_ms_clock());
                smoothaf_monitor_transition(&smoothaf_monitor,
                                            SMOOTHAF_MONITOR_PAUSED);
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
            smoothaf_monitor_accept(&smoothaf_monitor, &sample, completed);
            smoothaf_telemetry_update(valid ? score : -1, (int)sample.duration_ms,
                                      0, smoothaf_step, smoothaf_monitor.state,
                                      valid ? 0 : 1,
                                      valid ? 0 : SMOOTHAF_BLOCK_FRAME);
        }
        else msleep(20);
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
    int reason = smoothaf_measure_block_reason();
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_publish_measurement(-1, 0, 0, 0, 0, SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
    }
    else if (!smoothaf_measure(&score, &mean, &sad, &laplacian))
    {
        smoothaf_publish_measurement(-1, mean, sad, laplacian, 0,
                                     SMOOTHAF_MEASURE_ERROR);
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
    }
    else
    {
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
    smoothaf_normal_af_init(normal, initial_score, smoothaf_direction_value(),
                            smoothaf_step);
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
                step != smoothaf_step) return SMOOTHAF_RESULT_LIMIT;
            if (!smoothaf_move(direction)) return SMOOTHAF_RESULT_LENS_ERROR;
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
    policy.min_step = smoothaf_step;
    policy.initial_step = smoothaf_step;
    policy.max_step = smoothaf_step;
    smoothaf_adaptive_hc_init(adaptive, &policy, initial_score,
                              smoothaf_direction_value());
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
            if (!smoothaf_move(direction)) return SMOOTHAF_RESULT_LENS_ERROR;
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
    smoothaf_hc_init(&hc, 0, 1);
    smoothaf_status = SMOOTHAF_STATUS_MEASURING;
    smoothaf_last_commands = 0;
    smoothaf_last_reversals = 0;
    reason = smoothaf_block_reason();
    if (reason)
    {
        smoothaf_last_block = reason;
        smoothaf_last_error = 1;
        smoothaf_status = SMOOTHAF_STATUS_ERROR;
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
                                 ? SMOOTHAF_MEASURE_OK : SMOOTHAF_MEASURE_LOW);
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
    smoothaf_status = smoothaf_status_from_result(result);
    if (result == SMOOTHAF_RESULT_LENS_ERROR ||
        result == SMOOTHAF_RESULT_MEASURE_ERROR ||
        result == SMOOTHAF_RESULT_TIMEOUT || result == SMOOTHAF_RESULT_ERROR)
        smoothaf_last_error = 1;
done:
    smoothaf_last_time = (int)((uint32_t)get_ms_clock() - start);
    smoothaf_telemetry_update(smoothaf_last_score, smoothaf_last_time,
                              smoothaf_direction_value(), smoothaf_step,
                              smoothaf_status, smoothaf_last_error,
                              smoothaf_last_block);
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
        return;
    }
    if (smoothaf_is_task_running() || !smoothaf_task_exited)
    {
        smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_ERROR);
        return;
    }
    smoothaf_monitor_start(&smoothaf_monitor, smoothaf_metric,
                           (uint32_t)get_ms_clock());
    smoothaf_monitor_task_requested = 1;
    smoothaf_monitor_deinit = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 0;
    smoothaf_cancel = 0;
    if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))
    {
        smoothaf_monitor_stop(&smoothaf_monitor);
        smoothaf_monitor_task_requested = 0;
        smoothaf_monitor_transition(&smoothaf_monitor, SMOOTHAF_MONITOR_ERROR);
    }
}

static const char *smoothaf_monitor_task_state_name(void)
{
    if (smoothaf_task_running)
        return smoothaf_task_stop_requested ? "STOPPING" : "RUNNING";
    return smoothaf_task_exited ? "EXITED" : "STOPPED";
}

static MENU_UPDATE_FUNC(smoothaf_monitor_task_state_update)
{ MENU_SET_VALUE("%s", smoothaf_monitor_task_state_name()); }

static MENU_UPDATE_FUNC(smoothaf_monitor_configured_interval_update)
{ MENU_SET_VALUE("%d ms", (int)smoothaf_monitor_interval_ms(smoothaf_monitor.interval_index)); }

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
    MENU_SET_VALUE("%s", smoothaf_monitor.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR
                         ? "POSSIBLE_BLUR"
                         : smoothaf_monitor.state == SMOOTHAF_MONITOR_UNCERTAIN
                         ? "UNCERTAIN"
                         : smoothaf_monitor.state == SMOOTHAF_MONITOR_STABLE
                         ? "STABLE" : smoothaf_monitor.enabled ? "MONITORING" : "IDLE");
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
{ MENU_SET_VALUE("%d", smoothaf_monitor.reference); }
static MENU_UPDATE_FUNC(smoothaf_monitor_blur_update)
{ MENU_SET_VALUE("%s", smoothaf_monitor.possible_blur ? "YES" : "NO"); }

static MENU_SELECT_FUNC(smoothaf_stop_select)
{
    if (smoothaf_lock())
    {
        smoothaf_cancel = 1;
        smoothaf_task_stop_requested = 1;
        if (!smoothaf_task_running) smoothaf_status = SMOOTHAF_STATUS_IDLE;
        smoothaf_unlock();
    }
}

/* Emergency stop does not acquire smoothaf_sem: it remains usable while a
 * focus task owns the semaphore or is blocked in lens_focus(). */
static MENU_SELECT_FUNC(smoothaf_panic_select)
{
    smoothaf_panic_latch(&smoothaf_enabled, &smoothaf_cancel, &smoothaf_panic);
    smoothaf_task_stop_requested = 1;
    smoothaf_status = SMOOTHAF_STATUS_CANCELLED;
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
    MENU_SET_HELP("score=%d time=%dms dir=%d step=%d state=%d error=%d reason=%s",
                  smoothaf_telemetry.score, (int)smoothaf_telemetry.duration_ms,
                  (int)smoothaf_telemetry.direction, (int)smoothaf_telemetry.step,
                  (int)smoothaf_telemetry.state, (int)smoothaf_telemetry.error,
                  smoothaf_block_name(smoothaf_telemetry.reason));
}

static struct menu_entry smoothaf_menu[] =
{
    { .name = "Enable", .priv = &smoothaf_enabled, .max = 1,
      .help = "Enable lens movement for explicit focus actions." },
    { .name = "Vlog Monitor", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Start/Stop Monitoring", .select = smoothaf_monitor_toggle_select,
          .help = "Toggle periodic read-only monitoring; no automatic lens movement." },
        { .name = "Monitor Status", .update = smoothaf_monitor_status_update },
        { .name = "Monitor Performance", .select = menu_open_submenu,
          .children = (struct menu_entry[]){
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
        } },
        { .name = "SAD Score", .update = smoothaf_monitor_sad_update },
        { .name = "Laplacian Score", .update = smoothaf_monitor_laplacian_update },
        { .name = "Smoothed Score", .update = smoothaf_monitor_smoothed_update },
        { .name = "Reference Score", .update = smoothaf_monitor_reference_update },
        { .name = "Possible Blur", .update = smoothaf_monitor_blur_update },
        { .name = "Sample Count", .update = smoothaf_monitor_samples_update },
        { .name = "Sampling Interval", .priv = &smoothaf_monitor_interval,
          .min = SMOOTHAF_MONITOR_INTERVAL_1000, .max = SMOOTHAF_MONITOR_INTERVAL_100,
          .choices = CHOICES("1000 ms", "500 ms", "250 ms", "100 ms") },
        MENU_EOL,
    } },
    { .name = "Diagnostic", .select = menu_open_submenu,
      .children = (struct menu_entry[]){
        { .name = "Measure", .select = smoothaf_measure_select },
        { .name = "Focus Step", .select = smoothaf_step_select },
        { .name = "Direction", .priv = &smoothaf_direction_choice, .min = 0,
          .max = 1, .choices = CHOICES("Direction A", "Direction B") },
        MENU_EOL,
    } },
    { .name = "Focus Test", .select = smoothaf_search_select },
    { .name = "AF Mode", .priv = &smoothaf_af_mode, .min = SMOOTHAF_HC_LEGACY,
      .max = SMOOTHAF_AF_NORMAL,
      .choices = CHOICES("Legacy HC", "Adaptive HC", "Normal AF") },
    { .name = "Sharpness Metric", .priv = &smoothaf_metric,
      .min = SMOOTHAF_METRIC_SAD, .max = SMOOTHAF_METRIC_LAPLACIAN,
      .choices = CHOICES("SAD", "Laplacian") },
    { .name = "Step Size", .priv = &smoothaf_step, .min = 1, .max = 3,
      .unit = UNIT_DEC },
    { .name = "Status", .update = smoothaf_status_update },
    { .name = "Stop", .select = smoothaf_stop_select,
      .help = "Cancel future movement; a synchronous lens command may finish first." },
    { .name = "PANIC / Emergency Stop", .select = smoothaf_panic_select,
      .help = "Latch cancellation and disable focus; does not interrupt lens_focus()." },
    { .name = "Rearm Safe Mode", .select = smoothaf_rearm_select,
      .help = "Clear PANIC only after the previous task has ended; Enable remains off." },
};

static unsigned int smoothaf_init(void)
{
    smoothaf_sem = create_named_semaphore("smoothaf_sem", 1);
    if (!smoothaf_sem) return 1;
    smoothaf_enabled = 0;
    smoothaf_panic = 0;
    smoothaf_cancel = 0;
    smoothaf_task_running = 0;
    smoothaf_task_stop_requested = 0;
    smoothaf_task_exited = 1;
    smoothaf_status = SMOOTHAF_STATUS_IDLE;
    smoothaf_last_error = 0;
    smoothaf_last_block = SMOOTHAF_BLOCK_NONE;
    smoothaf_last_measure_code = SMOOTHAF_MEASURE_WAITING;
    smoothaf_monitor_init(&smoothaf_monitor, smoothaf_metric,
                          SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
    smoothaf_monitor_task_requested = 0;
    smoothaf_monitor_deinit = 0;
    smoothaf_telemetry_init(&smoothaf_telemetry);
    if (smoothaf_metric < SMOOTHAF_METRIC_SAD ||
        smoothaf_metric > SMOOTHAF_METRIC_LAPLACIAN) smoothaf_metric = SMOOTHAF_METRIC_SAD;
    if (smoothaf_step < 1 || smoothaf_step > 3) smoothaf_step = 1;
    if (smoothaf_direction_choice < 0 || smoothaf_direction_choice > 1)
        smoothaf_direction_choice = 0;
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
    MODULE_CONFIG(smoothaf_monitor_interval)
MODULE_CONFIGS_END()
