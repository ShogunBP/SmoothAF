#ifndef SMOOTHAF_MONITOR_H
#define SMOOTHAF_MONITOR_H

#include <stdint.h>
#include "smoothaf_core.h"
#include "smoothaf_algorithm.h"
#include "smoothaf_recovery.h"

#define SMOOTHAF_MONITOR_INTERVAL_1000 0
#define SMOOTHAF_MONITOR_INTERVAL_500  1
#define SMOOTHAF_MONITOR_INTERVAL_250  2
#define SMOOTHAF_MONITOR_INTERVAL_100  3
#define SMOOTHAF_MONITOR_INTERVAL_DEFAULT SMOOTHAF_MONITOR_INTERVAL_500

#define SMOOTHAF_MONITOR_IDLE          0
#define SMOOTHAF_MONITOR_MONITORING    1
#define SMOOTHAF_MONITOR_STABLE        2
#define SMOOTHAF_MONITOR_POSSIBLE_BLUR 3
#define SMOOTHAF_MONITOR_UNCERTAIN     4
#define SMOOTHAF_MONITOR_PAUSED        5
#define SMOOTHAF_MONITOR_ERROR         6

#define SMOOTHAF_MONITOR_METRIC_SAD       0
#define SMOOTHAF_MONITOR_METRIC_LAPLACIAN 1

#define SMOOTHAF_MONITOR_BLUR_ENTER_NUM 3
#define SMOOTHAF_MONITOR_BLUR_EXIT_NUM  7
#define SMOOTHAF_MONITOR_BLUR_DEN       4
#define SMOOTHAF_MONITOR_CONFIRM_SAMPLES 2
#define SMOOTHAF_MONITOR_LIGHT_DELTA 32

#define SMOOTHAF_VLOG_AF_OFF              0
#define SMOOTHAF_VLOG_AF_DRY_RUN          1
#define SMOOTHAF_VLOG_AF_ACTIVE           2
#define SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS 3

#define SMOOTHAF_VLOG_AF_WAITING   0
#define SMOOTHAF_VLOG_AF_PROBING   1
#define SMOOTHAF_VLOG_AF_SETTLING  2
#define SMOOTHAF_VLOG_AF_COOLDOWN  3
#define SMOOTHAF_VLOG_AF_COMPLETE  4
#define SMOOTHAF_VLOG_AF_ERROR     5

#define SMOOTHAF_VLOG_AF_MAX_COMMANDS   4
#define SMOOTHAF_VLOG_AF_MAX_REVERSALS  1
#define SMOOTHAF_VLOG_AF_COOLDOWN_MS    1000
#define SMOOTHAF_VLOG_AF_TIMEOUT_MS     5000
#define SMOOTHAF_VLOG_AF_MAX_CONSECUTIVE_FAILURES 2
#define SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT 4
#define SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT 1

#define SMOOTHAF_VLOG_AF_RESULT_NONE             0
#define SMOOTHAF_VLOG_AF_RESULT_API_SUCCESS      1
#define SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT      2
#define SMOOTHAF_VLOG_AF_RESULT_BLOCKED          3
#define SMOOTHAF_VLOG_AF_RESULT_FAILED           4
#define SMOOTHAF_VLOG_AF_RESULT_CANCELLED        5
#define SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT   6
#define SMOOTHAF_VLOG_AF_RESULT_DRY_RUN          7
#define SMOOTHAF_VLOG_AF_RESULT_ARMED            8
#define SMOOTHAF_VLOG_AF_RESULT_IMPROVED         9
#define SMOOTHAF_VLOG_AF_RESULT_WORSE           10
#define SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN       11
#define SMOOTHAF_VLOG_AF_RESULT_TIMEOUT         12
#define SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT    13
#define SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED  14
#define SMOOTHAF_VLOG_AF_RESULT_BEST_NOT_RECOVERED 15
#define SMOOTHAF_VLOG_AF_RESULT_CONVERGED       16

/* Reasons a command gate can deny an otherwise armed episode.  These are
 * diagnostic values; the safety limits themselves remain unchanged. */
#define SMOOTHAF_VLOG_AF_BLOCK_NONE             0
#define SMOOTHAF_VLOG_AF_BLOCK_OFF              1
#define SMOOTHAF_VLOG_AF_BLOCK_DISARMED         2
#define SMOOTHAF_VLOG_AF_BLOCK_DISABLED         3
#define SMOOTHAF_VLOG_AF_BLOCK_WAITING_RESULT   4
#define SMOOTHAF_VLOG_AF_BLOCK_STATE            5
#define SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT   6
#define SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT   7
#define SMOOTHAF_VLOG_AF_BLOCK_COOLDOWN         8
#define SMOOTHAF_VLOG_AF_BLOCK_SAFETY_GATE      9

/* Why a persistent ACTIVE CONTINUOUS session cannot start its next bounded
 * episode.  These values describe eligibility, not command safety limits. */
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_NONE          0
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_MODE          1
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISABLED      2
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISARMED      3
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_SAFETY_PAUSED 4
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACTIVE        5
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_MONITOR       6
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_COOLDOWN      7
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_BLUR_RECHECK  8

static inline const char *smoothaf_vlog_af_next_block_name(int reason)
{
    switch (reason)
    {
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_MODE: return "MODE";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISABLED: return "DISABLED";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISARMED: return "DISARMED";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_SAFETY_PAUSED: return "SAFETY_PAUSED";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACTIVE: return "EPISODE_ACTIVE";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_MONITOR: return "NO_CONFIRMED_BLUR";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_COOLDOWN: return "COOLDOWN";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_BLUR_RECHECK: return "BLUR_RECHECK";
        default: return "NONE";
    }
}

static inline const char *smoothaf_vlog_af_block_name(int reason)
{
    switch (reason)
    {
        case SMOOTHAF_VLOG_AF_BLOCK_OFF: return "OFF";
        case SMOOTHAF_VLOG_AF_BLOCK_DISARMED: return "DISARMED";
        case SMOOTHAF_VLOG_AF_BLOCK_DISABLED: return "DISABLED";
        case SMOOTHAF_VLOG_AF_BLOCK_WAITING_RESULT: return "WAITING_RESULT";
        case SMOOTHAF_VLOG_AF_BLOCK_STATE: return "STATE";
        case SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT: return "EPISODE_LIMIT";
        case SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT: return "SESSION_LIMIT";
        case SMOOTHAF_VLOG_AF_BLOCK_COOLDOWN: return "COOLDOWN";
        case SMOOTHAF_VLOG_AF_BLOCK_SAFETY_GATE: return "SAFETY_GATE";
        default: return "NONE";
    }
}

#define SMOOTHAF_VLOG_AF_SETTLE_MS      120
#define SMOOTHAF_VLOG_AF_BLUR_RECHECK_MS 500

#define SMOOTHAF_MOVIE_RECORDING_NONE   0
#define SMOOTHAF_MOVIE_RECORDING_H264   1
#define SMOOTHAF_MOVIE_RECORDING_CUSTOM 2
#define SMOOTHAF_MOVIE_RECORDING_RAW    3

static inline int smoothaf_movie_recording_class(int recording_h264,
                                                 int recording_custom,
                                                 int recording_raw)
{
    if (recording_raw) return SMOOTHAF_MOVIE_RECORDING_RAW;
    if (recording_custom) return SMOOTHAF_MOVIE_RECORDING_CUSTOM;
    if (recording_h264) return SMOOTHAF_MOVIE_RECORDING_H264;
    return SMOOTHAF_MOVIE_RECORDING_NONE;
}

struct smoothaf_movie_recording_state
{
    int active;
    int class_id;
    uint32_t started_ms;
    uint32_t ended_ms;
    uint32_t samples;
    uint32_t valid;
    uint32_t invalid;
};

static inline void smoothaf_movie_recording_init(
    struct smoothaf_movie_recording_state *state)
{
    state->active = 0;
    state->class_id = SMOOTHAF_MOVIE_RECORDING_NONE;
    state->started_ms = 0;
    state->ended_ms = 0;
    state->samples = 0;
    state->valid = 0;
    state->invalid = 0;
}

/* Return 1 for a new recording edge, 2 for an observed end edge, 0 otherwise. */
static inline int smoothaf_movie_recording_observe(
    struct smoothaf_movie_recording_state *state, int recording_class,
    uint32_t now)
{
    int active = recording_class != SMOOTHAF_MOVIE_RECORDING_NONE;
    if (active == state->active &&
        (!active || recording_class == state->class_id)) return 0;
    if (active)
    {
        state->active = 1;
        state->class_id = recording_class;
        state->started_ms = now;
        state->samples = 0;
        state->valid = 0;
        state->invalid = 0;
        return 1;
    }
    state->active = 0;
    /* Keep the last class visible after STOP so the menu and end event retain
     * whether the completed clip was H.264, custom or RAW. */
    state->ended_ms = now;
    return 2;
}

/* Movie Active is an explicitly authorized experiment.  It is deliberately
 * limited to normal H.264 Movie recording; RAW/custom remain blocked. */
static inline int smoothaf_movie_active_recording_allowed(
    int mode, int experimental, int liveview, int movie,
    int recording_h264, int recording_custom, int recording_raw)
{
    return experimental &&
           (mode == SMOOTHAF_VLOG_AF_ACTIVE ||
            mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) && liveview && movie &&
           recording_h264 && !recording_custom && !recording_raw;
}

static inline int smoothaf_movie_active_gate_allowed(
    int mode, int experimental, int liveview, int movie,
    int recording_h264, int recording_custom, int recording_raw, int cancelled,
    int manual_focus, int continuous_af, int follow_focus,
    int rack_focus, int focus_stack)
{
    return smoothaf_movie_active_recording_allowed(
               mode, experimental, liveview, movie, recording_h264,
               recording_custom, recording_raw) && !cancelled &&
           !manual_focus && !continuous_af && !follow_focus &&
           !rack_focus && !focus_stack;
}

/* Movie Dry Run is read-only and remains narrower than ACTIVE. */
static inline int smoothaf_movie_dry_run_recording_allowed(
    int mode, int liveview, int movie, int recording_h264,
    int recording_custom, int recording_raw)
{
    return mode == SMOOTHAF_VLOG_AF_DRY_RUN && liveview && movie &&
           recording_h264 && !recording_custom && !recording_raw;
}

static inline int smoothaf_movie_dry_run_gate_allowed(
    int mode, int liveview, int movie, int recording_h264,
    int recording_custom, int recording_raw, int cancelled,
    int manual_focus, int continuous_af, int follow_focus,
    int rack_focus, int focus_stack)
{
    return smoothaf_movie_dry_run_recording_allowed(
               mode, liveview, movie, recording_h264,
               recording_custom, recording_raw) && !cancelled &&
           !manual_focus && !continuous_af && !follow_focus &&
           !rack_focus && !focus_stack;
}

static inline const char *smoothaf_monitor_state_name(int state)
{
    switch (state)
    {
        case SMOOTHAF_MONITOR_MONITORING:    return "MONITORING";
        case SMOOTHAF_MONITOR_STABLE:        return "STABLE";
        case SMOOTHAF_MONITOR_POSSIBLE_BLUR: return "POSSIBLE_BLUR";
        case SMOOTHAF_MONITOR_UNCERTAIN:     return "UNCERTAIN";
        case SMOOTHAF_MONITOR_PAUSED:        return "PAUSED";
        case SMOOTHAF_MONITOR_ERROR:         return "ERROR";
        default:                             return "IDLE";
    }
}

static inline const char *smoothaf_vlog_af_result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_VLOG_AF_RESULT_API_SUCCESS:     return "API_SUCCESS";
        case SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT:     return "NO_MOVEMENT";
        case SMOOTHAF_VLOG_AF_RESULT_BLOCKED:         return "BLOCKED";
        case SMOOTHAF_VLOG_AF_RESULT_FAILED:          return "API_FAILED";
        case SMOOTHAF_VLOG_AF_RESULT_CANCELLED:       return "CANCELLED";
        case SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT:  return "NO_IMPROVEMENT";
        case SMOOTHAF_VLOG_AF_RESULT_DRY_RUN:         return "DRY_RUN";
        case SMOOTHAF_VLOG_AF_RESULT_ARMED:           return "ARMED";
        case SMOOTHAF_VLOG_AF_RESULT_IMPROVED:        return "IMPROVED";
        case SMOOTHAF_VLOG_AF_RESULT_WORSE:           return "WORSE";
        case SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN:       return "UNCERTAIN";
        case SMOOTHAF_VLOG_AF_RESULT_TIMEOUT:         return "TIMEOUT";
        case SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT:    return "SEARCH_LIMIT";
        case SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED:  return "BEST_RECOVERED";
        case SMOOTHAF_VLOG_AF_RESULT_BEST_NOT_RECOVERED: return "BEST_NOT_RECOVERED";
        case SMOOTHAF_VLOG_AF_RESULT_CONVERGED:       return "CONVERGED";
        default:                                      return "NONE";
    }
}

/* Arming is manual, but the monitor task itself is allowed to be running:
 * that task is the consumer of the episode, not a conflicting focus task. */
static inline int smoothaf_vlog_af_can_arm(int mode, int monitor_enabled,
                                           int task_running,
                                           int monitor_task_requested,
                                           int episode_armed)
{
    if (mode == SMOOTHAF_VLOG_AF_OFF) return 0;
    if (!monitor_enabled) return 0;
    if (episode_armed) return 0;
    if (task_running && !monitor_task_requested) return 0;
    return 1;
}

static inline int smoothaf_vlog_af_deadband(int best)
{
    int relative = best / 64;
    return relative > 24 ? relative : 24;
}

struct smoothaf_vlog_af
{
    int mode;
    int algorithm;
    int controller_initialized;
    int controller_step;
    int command_step;
    int controller_action;
    int controller_pending;
    int controller_measure_only;
    struct smoothaf_hc legacy_controller;
    struct smoothaf_adaptive_hc adaptive_controller;
    struct smoothaf_normal_af normal_controller;
    int state;
    int armed;
    int direction;
    int last_direction;
    int baseline;
    int best;
    uint32_t commands;
    uint32_t reversals;
    uint32_t session_commands;
    uint32_t generation;
    uint32_t settle_until_ms;
    uint32_t cooldown_until_ms;
    uint32_t blur_recheck_until_ms;
    uint32_t last_command_ms;
    int episode_limit;
    int consecutive_failures;
    int safety_paused;
    int continuous_armed;
    uint32_t episodes_completed;
    int wait_for_blur_clear;
    int cooldown_next_episode;
    int command_started;
    int awaiting_result;
    int dry_run_requests;
    int episode_result;
    int initial_score;
    int previous_score;
    int post_move_score;
    int score_delta;
    int best_position;
    int current_position;
    int controller_terminal_result;
    uint32_t started_ms;
    int uncertain;
    uint32_t image_ms;
    uint32_t wait_ms;
    uint32_t movement_ms;
    uint32_t stabilization_ms;
    uint32_t lens_calls;
    uint32_t last_result_ms;
    struct smoothaf_recovery recovery;
};

/* The passive monitor only promotes an armed episode after it has confirmed
 * blur.  A newly armed episode must stay WAITING until that evidence exists. */
static inline int smoothaf_vlog_af_should_probe(
    const struct smoothaf_vlog_af *af, int monitor_state, int mode)
{
    return mode != SMOOTHAF_VLOG_AF_OFF && af->armed &&
           af->state == SMOOTHAF_VLOG_AF_WAITING &&
           monitor_state == SMOOTHAF_MONITOR_POSSIBLE_BLUR;
}

static inline int smoothaf_vlog_af_should_start_continuous(
    const struct smoothaf_vlog_af *af, int monitor_state, int mode,
    int enabled)
{
    return mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && enabled &&
           af->continuous_armed && !af->armed && !af->safety_paused &&
           !af->wait_for_blur_clear &&
           monitor_state == SMOOTHAF_MONITOR_POSSIBLE_BLUR;
}

static inline int smoothaf_vlog_af_should_resume_continuous(
    const struct smoothaf_vlog_af *af, int monitor_state, int mode, int enabled,
    uint32_t now)
{
    /* The recheck deadline is the explicit exception to the blur-clear latch:
     * persistent blur may be retried after cooldown without requiring a
     * transient STABLE sample.  All other start gates remain mandatory. */
    return mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && enabled &&
           af->continuous_armed && !af->armed && !af->safety_paused &&
           monitor_state == SMOOTHAF_MONITOR_POSSIBLE_BLUR &&
           (!af->cooldown_next_episode ||
            (int32_t)(now - af->cooldown_until_ms) >= 0) &&
           (!af->wait_for_blur_clear ||
            (int32_t)(now - af->blur_recheck_until_ms) >= 0);
}

static inline int smoothaf_vlog_af_next_episode_block_reason(
    const struct smoothaf_vlog_af *af, int monitor_state, int mode, int enabled,
    uint32_t now)
{
    if (mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_MODE;
    if (!enabled) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISABLED;
    if (!af->continuous_armed) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISARMED;
    if (af->safety_paused) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_SAFETY_PAUSED;
    if (af->armed) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACTIVE;
    if (monitor_state != SMOOTHAF_MONITOR_POSSIBLE_BLUR)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_MONITOR;
    if (af->cooldown_next_episode &&
        (int32_t)(now - af->cooldown_until_ms) < 0)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_COOLDOWN;
    if (af->wait_for_blur_clear &&
        (int32_t)(now - af->blur_recheck_until_ms) < 0)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_BLUR_RECHECK;
    return SMOOTHAF_VLOG_AF_NEXT_BLOCK_NONE;
}

static inline void smoothaf_vlog_af_update_continuous_latch(
    struct smoothaf_vlog_af *af, int monitor_state, uint32_t now)
{
    if (monitor_state == SMOOTHAF_MONITOR_STABLE)
        af->wait_for_blur_clear = 0;
    if (af->cooldown_next_episode &&
        (int32_t)(now - af->cooldown_until_ms) >= 0)
        af->cooldown_next_episode = 0;
}

struct smoothaf_monitor_sample
{
    int score;
    int sad;
    int laplacian;
    int mean;
    int valid;
    int confident;
    int metric;
    uint32_t duration_ms;
};

struct smoothaf_monitor_metrics
{
    uint32_t attempts;
    uint32_t valid;
    uint32_t invalid;
    uint32_t low_confidence;
    uint32_t blocked;
    uint32_t skipped;
    uint32_t overruns;
    uint32_t duration_total;
    uint32_t duration_last;
    uint32_t duration_max;
    uint32_t duration_count;
    uint32_t actual_interval_ms;
    uint32_t previous_start_ms;
    uint32_t last_start_ms;
    uint32_t last_error;
    int have_start;
    int have_interval;
    uint32_t last_sample_ms;
    uint32_t next_due_ms;
};

struct smoothaf_monitor_snapshot
{
    int state;
    int enabled;
    int interval_index;
    uint32_t interval_ms;
    uint32_t actual_interval_ms;
    uint32_t duration_last;
    uint32_t duration_average;
    uint32_t duration_max;
    uint32_t attempts;
    uint32_t valid;
    uint32_t invalid;
    uint32_t low_confidence;
    uint32_t blocked;
    uint32_t skipped;
    uint32_t overruns;
    uint32_t last_error;
};

struct smoothaf_monitor
{
    int enabled;
    int state;
    int metric;
    int interval_index;
    int reference;
    int reference_mean;
    int score;
    int sad;
    int laplacian;
    int smoothed;
    int possible_blur;
    int blur_bad_count;
    int recovery_count;
    int lighting_count;
    int have_reference;
    int transition_pending;
    int previous_state;
    int transition_state;
    struct smoothaf_monitor_metrics metrics;
};

static inline void smoothaf_vlog_af_init(struct smoothaf_vlog_af *af)
{
    af->mode = SMOOTHAF_VLOG_AF_OFF;
    af->algorithm = SMOOTHAF_ALGORITHM_CURRENT_VLOG;
    af->controller_initialized = 0;
    af->controller_step = 1;
    af->command_step = 1;
    af->controller_action = 0;
    af->controller_pending = 0;
    af->controller_measure_only = 0;
    af->state = SMOOTHAF_VLOG_AF_WAITING;
    af->armed = 0;
    af->direction = 1;
    af->last_direction = 1;
    af->baseline = 0;
    af->best = 0;
    af->commands = 0;
    af->reversals = 0;
    af->session_commands = 0;
    af->generation = 0;
    af->settle_until_ms = 0;
    af->cooldown_until_ms = 0;
    af->blur_recheck_until_ms = 0;
    af->last_command_ms = 0;
    af->episode_limit = SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->consecutive_failures = 0;
    af->safety_paused = 0;
    af->continuous_armed = 0;
    af->episodes_completed = 0;
    af->wait_for_blur_clear = 0;
    af->cooldown_next_episode = 0;
    af->command_started = 0;
    af->awaiting_result = 0;
    af->dry_run_requests = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    af->initial_score = 0;
    af->previous_score = 0;
    af->post_move_score = 0;
    af->score_delta = 0;
    af->best_position = 0;
    af->current_position = 0;
    af->controller_terminal_result = 0;
    af->last_direction = 1;
    af->started_ms = 0;
    af->uncertain = 0;
    af->image_ms = 0;
    af->wait_ms = 0;
    af->movement_ms = 0;
    af->stabilization_ms = 0;
    af->lens_calls = 0;
    af->last_result_ms = 0;
    smoothaf_recovery_init(&af->recovery);
}

/* Reset operational authorization for a new monitor session without changing
 * the mode selected in the menu.  The selected mode is user preference; armed,
 * cooldown and safety fields are session state and must not survive STOP. */
static inline void smoothaf_vlog_af_reset_session(struct smoothaf_vlog_af *af,
                                                  int selected_mode)
{
    int algorithm = af->algorithm;
    smoothaf_vlog_af_init(af);
    af->mode = selected_mode;
    af->algorithm = smoothaf_algorithm_normalize(algorithm);
}

static inline void smoothaf_vlog_af_set_algorithm(struct smoothaf_vlog_af *af,
                                                  int algorithm)
{
    af->algorithm = smoothaf_algorithm_normalize(algorithm);
}

static inline void smoothaf_vlog_af_new_episode(struct smoothaf_vlog_af *af,
                                                int mode, int direction,
                                                int baseline)
{
    uint32_t completed = af->episodes_completed;
    int algorithm = af->algorithm;
    af->generation++;
    af->mode = mode;
    af->algorithm = smoothaf_algorithm_normalize(algorithm);
    af->state = SMOOTHAF_VLOG_AF_WAITING;
    af->armed = 1;
    af->continuous_armed = mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS;
    af->direction = direction < 0 ? -1 : 1;
    af->last_direction = af->direction;
    af->baseline = baseline;
    af->best = baseline;
    af->commands = 0;
    af->reversals = 0;
    af->settle_until_ms = 0;
    af->cooldown_until_ms = 0;
    af->blur_recheck_until_ms = 0;
    af->last_command_ms = 0;
    af->episode_limit = SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->consecutive_failures = 0;
    af->safety_paused = 0;
    af->episodes_completed = completed;
    af->wait_for_blur_clear = 0;
    af->cooldown_next_episode = 0;
    af->command_started = 0;
    af->awaiting_result = 0;
    af->dry_run_requests = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    af->controller_initialized = 0;
    af->controller_pending = 0;
    af->controller_step = 1;
    af->command_step = 1;
    af->initial_score = baseline;
    af->previous_score = baseline;
    af->post_move_score = 0;
    af->score_delta = 0;
    af->best_position = 0;
    af->current_position = 0;
    af->controller_terminal_result = 0;
    af->last_direction = af->direction;
    af->started_ms = 0;
    af->uncertain = 0;
    af->image_ms = 0;
    af->wait_ms = 0;
    af->movement_ms = 0;
    af->stabilization_ms = 0;
    af->lens_calls = 0;
    af->last_result_ms = 0;
    smoothaf_recovery_init(&af->recovery);
}

static inline void smoothaf_vlog_af_arm_continuous(
    struct smoothaf_vlog_af *af, int direction, int baseline)
{
    uint32_t completed = af->episodes_completed;
    smoothaf_vlog_af_new_episode(af, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS,
                                 direction, baseline);
    af->armed = 0;
    af->continuous_armed = 1;
    af->episodes_completed = completed;
    af->state = SMOOTHAF_VLOG_AF_WAITING;
}

static inline void smoothaf_vlog_af_set_episode_limit(
    struct smoothaf_vlog_af *af, int limit)
{
    af->episode_limit = limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT &&
                        limit <= SMOOTHAF_VLOG_AF_MAX_COMMANDS
                      ? limit : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
}

static inline void smoothaf_vlog_af_start_continuous_episode(
    struct smoothaf_vlog_af *af, int direction, int baseline)
{
    uint32_t completed = af->episodes_completed;
    int continuous = af->continuous_armed;
    int algorithm = af->algorithm;
    int failures = af->consecutive_failures;
    int safety_paused = af->safety_paused;
    int limit = af->episode_limit;
    smoothaf_vlog_af_new_episode(af, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS,
                                 direction, baseline);
    af->continuous_armed = continuous;
    af->episodes_completed = completed;
    af->consecutive_failures = failures;
    af->safety_paused = safety_paused;
    smoothaf_vlog_af_set_episode_limit(af, limit);
    af->algorithm = smoothaf_algorithm_normalize(algorithm);
}

static inline void smoothaf_vlog_af_complete_continuous_episode(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    af->armed = 0;
    af->awaiting_result = 0;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
    af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->blur_recheck_until_ms = now + SMOOTHAF_VLOG_AF_BLUR_RECHECK_MS;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
    if (af->episodes_completed != UINT32_MAX) af->episodes_completed++;
}

static inline uint32_t smoothaf_vlog_af_cooldown_remaining(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    if (!af->cooldown_next_episode ||
        (int32_t)(now - af->cooldown_until_ms) >= 0) return 0;
    return af->cooldown_until_ms - now;
}

static inline void smoothaf_vlog_af_stop(struct smoothaf_vlog_af *af)
{
    af->armed = 0;
    af->continuous_armed = 0;
    af->safety_paused = 1;
    af->awaiting_result = 0;
    if (af->episode_result == SMOOTHAF_VLOG_AF_RESULT_NONE ||
        af->episode_result == SMOOTHAF_VLOG_AF_RESULT_ARMED)
        af->episode_result = af->commands
                         ? SMOOTHAF_VLOG_AF_RESULT_CANCELLED
                         : SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
    smoothaf_recovery_init(&af->recovery);
}

static inline void smoothaf_vlog_af_record_measure(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    af->last_result_ms = now;
    af->awaiting_result = 1;
    af->settle_until_ms = now;
    af->state = SMOOTHAF_VLOG_AF_SETTLING;
}

static inline void smoothaf_vlog_af_record_command(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    if (af->commands != UINT32_MAX) af->commands++;
    if (af->session_commands != UINT32_MAX) af->session_commands++;
    if (!af->command_started)
    {
        af->started_ms = now;
        af->command_started = 1;
    }
    af->last_command_ms = now;
    af->awaiting_result = 1;
    if (af->wait_ms <= UINT32_MAX - SMOOTHAF_VLOG_AF_SETTLE_MS)
        af->wait_ms += SMOOTHAF_VLOG_AF_SETTLE_MS;
    af->state = SMOOTHAF_VLOG_AF_SETTLING;
    af->settle_until_ms = now + SMOOTHAF_VLOG_AF_SETTLE_MS;
    af->last_direction = af->direction;
}

/* COOLDOWN is advanced by the normal monitor cycle, not by the command gate.
 * This is a pure state update: it never records a command or moves the lens. */
static inline int smoothaf_vlog_af_advance_cooldown(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    if (!af->armed || af->state != SMOOTHAF_VLOG_AF_COOLDOWN) return 0;
    if ((int32_t)(now - af->cooldown_until_ms) < 0) return 0;
    af->state = SMOOTHAF_VLOG_AF_PROBING;
    return 1;
}

static inline int smoothaf_vlog_af_command_block_reason_for(
    const struct smoothaf_vlog_af *af, int mode, int enabled, uint32_t now)
{
    int effective_mode = af->mode == SMOOTHAF_VLOG_AF_DRY_RUN
                       ? SMOOTHAF_VLOG_AF_DRY_RUN : mode;
    if (!af->armed) return SMOOTHAF_VLOG_AF_BLOCK_DISARMED;
    if (effective_mode == SMOOTHAF_VLOG_AF_OFF)
        return SMOOTHAF_VLOG_AF_BLOCK_OFF;
    if ((af->mode == SMOOTHAF_VLOG_AF_ACTIVE ||
         af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) && af->safety_paused)
        return SMOOTHAF_VLOG_AF_BLOCK_SAFETY_GATE;
    if ((effective_mode == SMOOTHAF_VLOG_AF_ACTIVE ||
         effective_mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) && !enabled)
        return SMOOTHAF_VLOG_AF_BLOCK_DISABLED;
    if (af->awaiting_result) return SMOOTHAF_VLOG_AF_BLOCK_WAITING_RESULT;
    if (af->state != SMOOTHAF_VLOG_AF_PROBING) return SMOOTHAF_VLOG_AF_BLOCK_STATE;
    if (af->commands >= (uint32_t)(af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                                   ? af->episode_limit
                                   : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT))
        return SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT;
    if (!(af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed) &&
        af->session_commands >= SMOOTHAF_VLOG_AF_MAX_COMMANDS)
        return SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT;
    if ((af->cooldown_next_episode || af->cooldown_until_ms) &&
        (int32_t)(now - af->cooldown_until_ms) < 0)
        return SMOOTHAF_VLOG_AF_BLOCK_COOLDOWN;
    return SMOOTHAF_VLOG_AF_BLOCK_NONE;
}

static inline int smoothaf_vlog_af_command_block_reason(
    const struct smoothaf_vlog_af *af, int enabled, uint32_t now)
{
    return smoothaf_vlog_af_command_block_reason_for(af, af->mode, enabled, now);
}

static inline int smoothaf_vlog_af_can_command(
    struct smoothaf_vlog_af *af, int enabled, uint32_t now)
{
    return smoothaf_vlog_af_command_block_reason(af, enabled, now) ==
           SMOOTHAF_VLOG_AF_BLOCK_NONE;
}

static inline uint32_t smoothaf_vlog_af_commands_available(
    const struct smoothaf_vlog_af *af)
{
    uint32_t limit = af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                   ? (uint32_t)af->episode_limit
                   : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    uint32_t episode = limit - (af->commands < limit ? af->commands : limit);
    uint32_t session;
    if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
        return episode;
    session = SMOOTHAF_VLOG_AF_MAX_COMMANDS -
              (af->session_commands < SMOOTHAF_VLOG_AF_MAX_COMMANDS
               ? af->session_commands : SMOOTHAF_VLOG_AF_MAX_COMMANDS);
    return episode < session ? episode : session;
}

/* Overall episode timeout, checked between monitor samples.  An armed episode
 * that has not issued a command yet is not timed out. */
static inline int smoothaf_vlog_af_timeout_due(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    return af->armed && af->command_started &&
           (uint32_t)(now - af->started_ms) > SMOOTHAF_VLOG_AF_TIMEOUT_MS;
}

static inline void smoothaf_vlog_af_pause_continuous(
    struct smoothaf_vlog_af *af, int result, uint32_t now)
{
    af->armed = 0;
    af->awaiting_result = 0;
    af->episode_result = result;
    if (af->consecutive_failures != INT32_MAX) af->consecutive_failures++;
    af->state = SMOOTHAF_VLOG_AF_ERROR;
    af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
    if (af->consecutive_failures >= SMOOTHAF_VLOG_AF_MAX_CONSECUTIVE_FAILURES)
        af->safety_paused = 1;
}

static inline uint32_t smoothaf_vlog_af_timeout_remaining(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    uint32_t elapsed;
    if (!af->armed) return 0;
    if (!af->command_started) return SMOOTHAF_VLOG_AF_TIMEOUT_MS;
    elapsed = now - af->started_ms;
    return elapsed >= SMOOTHAF_VLOG_AF_TIMEOUT_MS
         ? 0 : SMOOTHAF_VLOG_AF_TIMEOUT_MS - elapsed;
}

/* Switching the menu mode to OFF must not leave an armed episode able to
 * command the lens under the mode captured when it was armed. */
static inline int smoothaf_vlog_af_should_cancel(
    const struct smoothaf_vlog_af *af, int mode)
{
    return (af->armed || af->continuous_armed) && mode != af->mode;
}

/* An episode armed as DRY RUN must stay dry even if the menu mode changes,
 * and switching a live episode to DRY RUN must stop physical commands. */
static inline int smoothaf_vlog_af_uses_dry_run(
    const struct smoothaf_vlog_af *af, int mode)
{
    return af->mode == SMOOTHAF_VLOG_AF_DRY_RUN ||
           mode == SMOOTHAF_VLOG_AF_DRY_RUN;
}

static inline int smoothaf_vlog_af_result_due(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    return af->awaiting_result &&
           (int32_t)(now - af->settle_until_ms) >= 0;
}

static inline int smoothaf_vlog_af_dry_run_request(
    struct smoothaf_vlog_af *af, int enabled, uint32_t now)
{
    if (af->mode != SMOOTHAF_VLOG_AF_DRY_RUN ||
        !smoothaf_vlog_af_can_command(af, enabled, now))
        return 0;
    if (af->dry_run_requests != INT32_MAX) af->dry_run_requests++;
    smoothaf_vlog_af_record_command(af, now);
    return 1;
}

static inline int smoothaf_vlog_af_record_result(
    struct smoothaf_vlog_af *af, int score, uint32_t now)
{
    int delta = score - af->previous_score;
    int deadband = smoothaf_vlog_af_deadband(af->best);
    int limit = af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
              ? af->episode_limit : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->post_move_score = score;
    af->last_result_ms = now;
    af->score_delta = delta;
    af->awaiting_result = 0;
    af->consecutive_failures = 0;
    if (score > af->best)
    {
        af->best = score;
        af->best_position = af->current_position;
    }
    if (delta > deadband)
    {
        af->previous_score = score;
        af->episode_result = SMOOTHAF_VLOG_AF_RESULT_IMPROVED;
        af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
        af->state = SMOOTHAF_VLOG_AF_COOLDOWN;
        if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
        {
            /* Improvement is not convergence.  Keep the continuous consent
             * alive, but finish this bounded episode and wait for fresh blur
             * evidence before starting another one. */
            if (af->commands >= (uint32_t)limit)
                af->episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
            else
                af->episode_result = SMOOTHAF_VLOG_AF_RESULT_CONVERGED;
            smoothaf_vlog_af_complete_continuous_episode(af, now);
            return 1;
        }
        if (af->commands >= (uint32_t)limit)
        {
            af->armed = 0;
            af->state = SMOOTHAF_VLOG_AF_COMPLETE;
            af->cooldown_next_episode = 1;
            af->wait_for_blur_clear = 1;
            af->episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
        }
        return 1;
    }
    if (delta < -deadband)
    {
        af->episode_result = SMOOTHAF_VLOG_AF_RESULT_WORSE;
        if (af->commands >= (uint32_t)limit)
        {
            af->episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
            if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
                smoothaf_vlog_af_complete_continuous_episode(af, now);
            else
            {
                af->armed = 0;
                af->state = SMOOTHAF_VLOG_AF_COMPLETE;
            }
            return 0;
        }
        if (af->reversals < SMOOTHAF_VLOG_AF_MAX_REVERSALS)
        {
            af->reversals++;
            af->direction = -af->direction;
            af->state = SMOOTHAF_VLOG_AF_PROBING;
            return 0;
        }
    }
    else
    {
        af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT;
    }
    if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
    {
        smoothaf_vlog_af_complete_continuous_episode(af, now);
        return 0;
    }
    af->armed = 0;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
    af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
    return 0;
}

static inline uint32_t smoothaf_monitor_interval_ms(int index)
{
    switch (index)
    {
        case SMOOTHAF_MONITOR_INTERVAL_1000: return 1000;
        case SMOOTHAF_MONITOR_INTERVAL_250:  return 250;
        case SMOOTHAF_MONITOR_INTERVAL_100:  return 100;
        case SMOOTHAF_MONITOR_INTERVAL_500:  return 500;
        default: return 500;
    }
}

static inline int smoothaf_monitor_normalize_interval(int index)
{
    return (index >= SMOOTHAF_MONITOR_INTERVAL_1000 &&
            index <= SMOOTHAF_MONITOR_INTERVAL_100) ? index
                                                     : SMOOTHAF_MONITOR_INTERVAL_DEFAULT;
}

static inline int smoothaf_monitor_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static inline void smoothaf_monitor_transition(struct smoothaf_monitor *m, int state)
{
    if (m->state == state) return;
    m->previous_state = m->state;
    m->state = state;
    m->transition_state = state;
    m->transition_pending = 1;
}

static inline void smoothaf_monitor_init(struct smoothaf_monitor *m, int metric,
                                         int interval_index)
{
    m->enabled = 0;
    m->state = SMOOTHAF_MONITOR_IDLE;
    m->metric = metric;
    m->interval_index = smoothaf_monitor_normalize_interval(interval_index);
    m->reference = 0;
    m->reference_mean = 0;
    m->score = 0;
    m->sad = 0;
    m->laplacian = 0;
    m->smoothed = 0;
    m->possible_blur = 0;
    m->blur_bad_count = 0;
    m->recovery_count = 0;
    m->lighting_count = 0;
    m->have_reference = 0;
    m->transition_pending = 0;
    m->previous_state = SMOOTHAF_MONITOR_IDLE;
    m->transition_state = SMOOTHAF_MONITOR_IDLE;
    m->metrics.attempts = 0;
    m->metrics.valid = 0;
    m->metrics.invalid = 0;
    m->metrics.low_confidence = 0;
    m->metrics.blocked = 0;
    m->metrics.skipped = 0;
    m->metrics.overruns = 0;
    m->metrics.duration_total = 0;
    m->metrics.duration_last = 0;
    m->metrics.duration_max = 0;
    m->metrics.duration_count = 0;
    m->metrics.actual_interval_ms = 0;
    m->metrics.previous_start_ms = 0;
    m->metrics.last_start_ms = 0;
    m->metrics.last_error = 0;
    m->metrics.have_start = 0;
    m->metrics.have_interval = 0;
    m->metrics.last_sample_ms = 0;
    m->metrics.next_due_ms = 0;
}

static inline void smoothaf_monitor_start(struct smoothaf_monitor *m, int metric,
                                          uint32_t now)
{
    int interval = m->interval_index;
    smoothaf_monitor_init(m, metric, interval);
    m->enabled = 1;
    m->state = SMOOTHAF_MONITOR_MONITORING;
    m->transition_state = SMOOTHAF_MONITOR_MONITORING;
    m->transition_pending = 1;
    m->metrics.next_due_ms = now;
}

static inline void smoothaf_monitor_stop(struct smoothaf_monitor *m)
{
    m->enabled = 0;
    smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_IDLE);
}

static inline int smoothaf_monitor_take_transition(struct smoothaf_monitor *m,
                                                   int *from, int *to)
{
    if (!m->transition_pending) return 0;
    if (from) *from = m->previous_state;
    if (to) *to = m->transition_state;
    m->transition_pending = 0;
    return 1;
}

static inline void smoothaf_monitor_record_blocked(struct smoothaf_monitor *m)
{
    if (m->metrics.blocked != UINT32_MAX) m->metrics.blocked++;
}

static inline void smoothaf_monitor_record_skip(struct smoothaf_monitor *m)
{
    if (m->metrics.skipped != UINT32_MAX) m->metrics.skipped++;
}

static inline void smoothaf_monitor_record_error(struct smoothaf_monitor *m,
                                                 uint32_t error)
{
    m->metrics.last_error = error;
}

static inline void smoothaf_monitor_record_start(struct smoothaf_monitor *m,
                                                 uint32_t started_at)
{
    if (m->metrics.have_start)
    {
        m->metrics.actual_interval_ms = started_at - m->metrics.last_start_ms;
        m->metrics.have_interval = 1;
    }
    m->metrics.previous_start_ms = m->metrics.last_start_ms;
    m->metrics.last_start_ms = started_at;
    m->metrics.have_start = 1;
}

static inline void smoothaf_monitor_record_duration(struct smoothaf_monitor *m,
                                                    uint32_t duration_ms)
{
    m->metrics.duration_last = duration_ms;
    if (m->metrics.duration_max < duration_ms)
        m->metrics.duration_max = duration_ms;
    if (m->metrics.duration_total <= UINT32_MAX - duration_ms)
        m->metrics.duration_total += duration_ms;
    else
        m->metrics.duration_total = UINT32_MAX;
    if (m->metrics.duration_count != UINT32_MAX)
        m->metrics.duration_count++;
}

static inline uint32_t smoothaf_monitor_duration_average(
    const struct smoothaf_monitor *m)
{
    if (!m->metrics.duration_count) return 0;
    return m->metrics.duration_total / m->metrics.duration_count;
}

static inline void smoothaf_monitor_snapshot(
    const struct smoothaf_monitor *m, struct smoothaf_monitor_snapshot *s)
{
    s->state = m->state;
    s->enabled = m->enabled;
    s->interval_index = m->interval_index;
    s->interval_ms = smoothaf_monitor_interval_ms(m->interval_index);
    s->actual_interval_ms = m->metrics.actual_interval_ms;
    s->duration_last = m->metrics.duration_last;
    s->duration_average = smoothaf_monitor_duration_average(m);
    s->duration_max = m->metrics.duration_max;
    s->attempts = m->metrics.attempts;
    s->valid = m->metrics.valid;
    s->invalid = m->metrics.invalid;
    s->low_confidence = m->metrics.low_confidence;
    s->blocked = m->metrics.blocked;
    s->skipped = m->metrics.skipped;
    s->overruns = m->metrics.overruns;
    s->last_error = m->metrics.last_error;
}

static inline void smoothaf_monitor_schedule_after(struct smoothaf_monitor *m,
                                                   uint32_t completed_at)
{
    m->metrics.next_due_ms = completed_at +
                             smoothaf_monitor_interval_ms(m->interval_index);
}

static inline void smoothaf_monitor_accept(struct smoothaf_monitor *m,
                                           const struct smoothaf_monitor_sample *s,
                                           uint32_t now)
{
    int lighting;
    int low;
    smoothaf_monitor_record_duration(m, s->duration_ms);
    if (!s->valid)
    {
        if (m->metrics.invalid != UINT32_MAX) m->metrics.invalid++;
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    if (m->metrics.valid != UINT32_MAX) m->metrics.valid++;
    m->score = s->score;
    m->sad = s->sad;
    m->laplacian = s->laplacian;
    m->metrics.last_sample_ms = now;
    low = !s->confident;
    if (low && m->metrics.low_confidence != UINT32_MAX)
        m->metrics.low_confidence++;
    if (low)
    {
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    if (!m->have_reference || m->metric != s->metric)
    {
        m->metric = s->metric;
        m->reference = s->score;
        m->reference_mean = s->mean;
        m->smoothed = s->score;
        m->have_reference = 1;
        m->blur_bad_count = 0;
        m->recovery_count = 0;
        m->lighting_count = 0;
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    lighting = s->mean - m->reference_mean;
    if (lighting < 0) lighting = -lighting;
    if (lighting >= SMOOTHAF_MONITOR_LIGHT_DELTA)
    {
        m->lighting_count++;
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    m->smoothed = (m->smoothed * 3 + s->score) / 4;
    if (m->smoothed * SMOOTHAF_MONITOR_BLUR_DEN <
        m->reference * SMOOTHAF_MONITOR_BLUR_ENTER_NUM)
    {
        m->blur_bad_count++;
        m->recovery_count = 0;
        if (m->blur_bad_count >= SMOOTHAF_MONITOR_CONFIRM_SAMPLES)
        {
            m->possible_blur = 1;
            smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_POSSIBLE_BLUR);
        }
    }
    else if (m->smoothed * SMOOTHAF_MONITOR_BLUR_DEN >=
             m->reference * SMOOTHAF_MONITOR_BLUR_EXIT_NUM / 2)
    {
        m->recovery_count++;
        m->blur_bad_count = 0;
        if (m->recovery_count >= SMOOTHAF_MONITOR_CONFIRM_SAMPLES)
        {
            m->possible_blur = 0;
            smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);
        }
    }
    else
    {
        m->recovery_count = 0;
    }

    if (!m->possible_blur && m->state == SMOOTHAF_MONITOR_STABLE &&
        s->score >= m->reference)
    {
        int delta = (s->score - m->reference) / 16;
        if (delta > 0) m->reference += delta;
    }
    smoothaf_monitor_schedule_after(m, now);
}

#endif
