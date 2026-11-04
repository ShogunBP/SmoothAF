#ifndef SMOOTHAF_MONITOR_H
#define SMOOTHAF_MONITOR_H

#include <stdint.h>
#include "smoothaf_core.h"
#include "smoothaf_algorithm.h"
#include "smoothaf_direction.h"
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
#define SMOOTHAF_MONITOR_LIGHT_EXIT_DELTA 24
#define SMOOTHAF_MONITOR_REF_RECALIBRATE_SAMPLES 3

/* A reference is not focus evidence just because the first confident sample
 * arrived.  PROVISIONAL keeps a baseline for telemetry and one bounded AF
 * acquisition, but it can never authorize blur detection by itself. */
#define SMOOTHAF_MONITOR_REF_VALID       0
#define SMOOTHAF_MONITOR_REF_OBSOLETE    1
#define SMOOTHAF_MONITOR_REF_RECALIBRATE 2
#define SMOOTHAF_MONITOR_REF_PROVISIONAL 3

#define SMOOTHAF_MONITOR_REASON_INITIAL          1
#define SMOOTHAF_MONITOR_REASON_METRIC           2
#define SMOOTHAF_MONITOR_REASON_LIGHT            3
#define SMOOTHAF_MONITOR_REASON_RECALIBRATE      4
#define SMOOTHAF_MONITOR_REASON_AF_RESULT        5
#define SMOOTHAF_MONITOR_REASON_PROVISIONAL_RECAL 6

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

/* The menu limit applies to the selected algorithm.  AUTO is a bounded
 * pre-search and has its own allowance; the sum remains capped absolutely. */
#define SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT 4
#define SMOOTHAF_VLOG_AF_MAX_COMMANDS   12
#define SMOOTHAF_VLOG_AF_MAX_RECOVERY_COMMANDS SMOOTHAF_RECOVERY_MAX_COMMANDS
#define SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS \
    (SMOOTHAF_VLOG_AF_MAX_COMMANDS + SMOOTHAF_AUTO_MAX_COMMANDS + \
     SMOOTHAF_VLOG_AF_MAX_RECOVERY_COMMANDS)
#define SMOOTHAF_VLOG_AF_MAX_REVERSALS  1
#define SMOOTHAF_VLOG_AF_COOLDOWN_MS    1000
#define SMOOTHAF_VLOG_AF_TIMEOUT_MS     8000
#define SMOOTHAF_VLOG_AF_MAX_CONSECUTIVE_FAILURES 2
#define SMOOTHAF_VLOG_AF_MAX_NEUTRAL_LIMIT_STREAK 3
#define SMOOTHAF_VLOG_AF_NEUTRAL_BACKOFF_MS 2000
#define SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT 1

/* Initial acquisition is bounded per provisional epoch and per manual arm.
 * A failed attempt may be retried only after cooldown/backoff, and the whole
 * series remains capped so a flat or low-texture scene cannot hunt forever. */
#define SMOOTHAF_VLOG_AF_MAX_ACQUISITION_ATTEMPTS 3
#define SMOOTHAF_VLOG_AF_MAX_SESSION_ACQUISITIONS 8
#define SMOOTHAF_VLOG_AF_ACQUIRE_BACKOFF_MS 2000

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
#define SMOOTHAF_VLOG_AF_BLOCK_DEADLINE         10

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
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_REFERENCE     9
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_LIMIT 10
#define SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_EXHAUSTED 11

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
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_REFERENCE: return "REFERENCE";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_LIMIT: return "ACQUIRE_LIMIT";
        case SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_EXHAUSTED: return "ACQUIRE_EXHAUSTED";
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
        case SMOOTHAF_VLOG_AF_BLOCK_DEADLINE: return "DEADLINE";
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

static inline const char *smoothaf_monitor_reference_state_name(int state)
{
    switch (state)
    {
        case SMOOTHAF_MONITOR_REF_VALID:        return "VALID";
        case SMOOTHAF_MONITOR_REF_OBSOLETE:     return "OBSOLETE";
        case SMOOTHAF_MONITOR_REF_RECALIBRATE:  return "RECALIBRATE";
        case SMOOTHAF_MONITOR_REF_PROVISIONAL:  return "PROVISIONAL";
        default:                                return "RECALIBRATE";
    }
}

static inline const char *smoothaf_monitor_reference_reason_name(int reason)
{
    switch (reason)
    {
        case SMOOTHAF_MONITOR_REASON_INITIAL:          return "INIT";
        case SMOOTHAF_MONITOR_REASON_METRIC:           return "METRIC";
        case SMOOTHAF_MONITOR_REASON_LIGHT:            return "LIGHT";
        case SMOOTHAF_MONITOR_REASON_RECALIBRATE:      return "RECAL";
        case SMOOTHAF_MONITOR_REASON_AF_RESULT:        return "AF";
        case SMOOTHAF_MONITOR_REASON_PROVISIONAL_RECAL: return "RECAL_PROV";
        default:                                       return "NONE";
    }
}

/* Menu-facing task state is deliberately derived from the task lifecycle,
 * not from the last SET action.  Transitional and failed states must never
 * look like a running monitor. */
static inline const char *smoothaf_monitor_task_state_name_for(
    int task_running, int stop_requested, int task_exited, int monitor_error)
{
    if (task_running) return stop_requested ? "STOPPING" : "RUNNING";
    if (monitor_error) return "ERROR";
    if (!task_exited) return "STARTING";
    return "STOPPED";
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
    int auto_probe_done;
    int auto_probe_result;
    int auto_probe_commands;
    int auto_probe_reversals;
    int auto_probe_phase;
    int auto_probe_first_direction;
    int auto_probe_selected_direction;
    int auto_probe_first_score;
    int auto_probe_opposite_score;
    int auto_probe_first_delta;
    int auto_probe_opposite_delta;
    int auto_probe_enabled;
    /* Luminance range observed across the samples that selected the AUTO
     * direction.  A range at or above the lighting threshold makes the score
     * comparison unreliable and blocks reference qualification. */
    int auto_probe_min_mean;
    int auto_probe_max_mean;
    int auto_probe_lighting_valid;
    int auto_probe_lighting_observed;
    uint32_t algorithm_commands;
    uint32_t algorithm_command_limit;
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
    int neutral_limit_streak;
    int safety_paused;
    int continuous_armed;
    uint32_t episodes_completed;
    int wait_for_blur_clear;
    int cooldown_next_episode;
    int command_started;
    int awaiting_result;
    /* Bounded acquisition series per provisional-reference epoch.  The epoch
     * identifies the scene, the attempt counter bounds retries, and the
     * session counter bounds a whole manual arm. */
    int acquisition_epoch;
    int acquisition_attempts;
    int session_acquisitions;
    int acquisition_attempt_active;
    int recovery_reserved_commands;
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
    uint32_t deadline_ms;
    int uncertain;
    uint32_t image_ms;
    uint32_t wait_ms;
    uint32_t movement_ms;
    uint32_t stabilization_ms;
    uint32_t total_lens_ms;
    uint32_t total_settle_ms;
    uint32_t total_sample_wait_ms;
    uint32_t lens_calls;
    uint32_t last_result_ms;
    uint32_t last_lens_start_ms;
    uint32_t last_lens_end_ms;
    uint32_t last_settle_start_ms;
    uint32_t last_sample_ms;
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

static inline int smoothaf_vlog_af_auto_selected(
    const struct smoothaf_vlog_af *af, int direction_setting)
{
    return af->algorithm >= SMOOTHAF_ALGORITHM_CURRENT_VLOG &&
           smoothaf_vlog_direction_is_auto(direction_setting);
}

static inline void smoothaf_vlog_af_set_auto_probe(
    struct smoothaf_vlog_af *af, int enabled)
{
    af->auto_probe_enabled = enabled ? 1 : 0;
}

static inline void smoothaf_vlog_af_reserve_recovery(
    struct smoothaf_vlog_af *af, int reserved)
{
    af->recovery_reserved_commands = reserved ? 1 : 0;
}

static inline void smoothaf_vlog_af_auto_begin_measurement(
    struct smoothaf_vlog_af *af, int mean)
{
    af->auto_probe_min_mean = mean;
    af->auto_probe_max_mean = mean;
    af->auto_probe_lighting_valid = 1;
    af->auto_probe_lighting_observed = 1;
}

static inline void smoothaf_vlog_af_auto_observe_mean(
    struct smoothaf_vlog_af *af, int mean)
{
    int min_mean;
    int max_mean;
    if (af->auto_probe_min_mean == 0 && af->auto_probe_max_mean == 0)
    {
        smoothaf_vlog_af_auto_begin_measurement(af, mean);
        return;
    }
    if (mean < af->auto_probe_min_mean) af->auto_probe_min_mean = mean;
    if (mean > af->auto_probe_max_mean) af->auto_probe_max_mean = mean;
    min_mean = af->auto_probe_min_mean;
    max_mean = af->auto_probe_max_mean;
    af->auto_probe_lighting_observed = 1;
    if (max_mean - min_mean >= SMOOTHAF_MONITOR_LIGHT_DELTA)
        af->auto_probe_lighting_valid = 0;
}

static inline uint32_t smoothaf_vlog_af_episode_command_limit(
    const struct smoothaf_vlog_af *af)
{
    uint32_t algorithm = af->algorithm_command_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                       ? af->algorithm_command_limit
                       : (af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                          ? (uint32_t)af->episode_limit
                          : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT);
    uint32_t probe = af->auto_probe_enabled ? SMOOTHAF_AUTO_MAX_COMMANDS : 0;
    uint32_t recovery = af->recovery_reserved_commands
                      ? SMOOTHAF_RECOVERY_MAX_COMMANDS : 0;
    uint32_t total = algorithm + probe + recovery;
    return total <= SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS
         ? total : SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS;
}

static inline uint32_t smoothaf_vlog_af_algorithm_commands_used(
    const struct smoothaf_vlog_af *af)
{
    return af->auto_probe_enabled ? af->algorithm_commands : af->commands;
}

static inline uint32_t smoothaf_vlog_af_session_command_limit_for(
    const struct smoothaf_vlog_af *af, int auto_probe_enabled)
{
    uint32_t limit = af->algorithm_command_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                   ? af->algorithm_command_limit
                   : (af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                      ? (uint32_t)af->episode_limit
                      : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT);
    if (auto_probe_enabled) limit += SMOOTHAF_AUTO_MAX_COMMANDS;
    if (af->recovery_reserved_commands) limit += SMOOTHAF_RECOVERY_MAX_COMMANDS;
    return limit <= SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS
         ? limit : SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS;
}

static inline uint32_t smoothaf_vlog_af_session_command_limit(
    const struct smoothaf_vlog_af *af)
{
    return smoothaf_vlog_af_session_command_limit_for(af,
                                                       af->auto_probe_enabled);
}

static inline uint32_t smoothaf_vlog_af_algorithm_command_limit(
    const struct smoothaf_vlog_af *af)
{
    return af->algorithm_command_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
         ? af->algorithm_command_limit
         : (af->episode_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
            ? (uint32_t)af->episode_limit
            : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT);
}

static inline uint32_t smoothaf_vlog_af_algorithm_commands_available(
    const struct smoothaf_vlog_af *af)
{
    uint32_t limit = smoothaf_vlog_af_algorithm_command_limit(af);
    uint32_t used = smoothaf_vlog_af_algorithm_commands_used(af);
    return used < limit ? limit - used : 0;
}

static inline int smoothaf_vlog_af_auto_needs_move(
    struct smoothaf_vlog_af *af, int direction_setting, int probe_step,
    int *direction, int *step)
{
    if (!smoothaf_vlog_af_auto_selected(af, direction_setting) ||
        af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_DONE ||
        af->auto_probe_commands >= SMOOTHAF_AUTO_MAX_COMMANDS)
        return 0;
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_IDLE)
    {
        af->auto_probe_phase = SMOOTHAF_VLOG_AUTO_FIRST_MOVE;
        af->auto_probe_first_direction =
            (af->generation & 1) ? 1 : -1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_FIRST_MOVE)
    {
        if (direction) *direction = af->auto_probe_first_direction;
        if (step) *step = probe_step;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_FIRST_RETURN)
    {
        if (direction) *direction = -af->auto_probe_first_direction;
        if (step) *step = probe_step;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_OPPOSITE_MOVE)
    {
        if (direction) *direction = -af->auto_probe_first_direction;
        if (step) *step = probe_step;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_OPPOSITE_RETURN)
    {
        if (direction) *direction = af->auto_probe_first_direction;
        if (step) *step = probe_step;
        return 1;
    }
    return 0;
}

static inline void smoothaf_vlog_af_auto_commit_move(
    struct smoothaf_vlog_af *af)
{
    if (af->auto_probe_commands != INT32_MAX)
        af->auto_probe_commands++;
}

static inline int smoothaf_vlog_af_auto_on_score(
    struct smoothaf_vlog_af *af, int score)
{
    int delta = score - af->initial_score;
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_FIRST_MOVE)
    {
        af->auto_probe_first_score = score;
        af->auto_probe_first_delta = delta;
        af->auto_probe_phase = SMOOTHAF_VLOG_AUTO_FIRST_RETURN;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_FIRST_RETURN)
    {
        af->auto_probe_phase = SMOOTHAF_VLOG_AUTO_OPPOSITE_MOVE;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_OPPOSITE_MOVE)
    {
        af->auto_probe_opposite_score = score;
        af->auto_probe_opposite_delta = delta;
        af->auto_probe_phase = SMOOTHAF_VLOG_AUTO_OPPOSITE_RETURN;
        return 1;
    }
    if (af->auto_probe_phase == SMOOTHAF_VLOG_AUTO_OPPOSITE_RETURN)
    {
        int db = smoothaf_auto_deadband(af->initial_score);
        if ((af->auto_probe_lighting_observed &&
             !af->auto_probe_lighting_valid) ||
            (af->auto_probe_first_delta <= db &&
             af->auto_probe_opposite_delta <= db))
            af->auto_probe_selected_direction = 0;
        else
            af->auto_probe_selected_direction =
                af->auto_probe_opposite_delta > af->auto_probe_first_delta
              ? -af->auto_probe_first_direction : af->auto_probe_first_direction;
        af->direction = af->auto_probe_selected_direction
                      ? af->auto_probe_selected_direction
                      : af->auto_probe_first_direction;
        af->auto_probe_result = af->auto_probe_selected_direction
                              ? (af->auto_probe_selected_direction == af->auto_probe_first_direction
                                 ? SMOOTHAF_AUTO_FIRST_IMPROVED
                                 : SMOOTHAF_AUTO_OPPOSITE_IMPROVED)
                              : SMOOTHAF_AUTO_UNCERTAIN;
        af->auto_probe_phase = SMOOTHAF_VLOG_AUTO_DONE;
        af->auto_probe_done = 1;
        return 0;
    }
    return 0;
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

/* Initial acquisition is a separate, explicitly bounded authorization.  It is
 * consumed once per provisional-reference epoch, so the same unfocused scene
 * cannot generate an unlimited sequence of lens commands. */
static inline int smoothaf_vlog_af_should_acquire_continuous(
    const struct smoothaf_vlog_af *af, int reference_state, int reference_epoch,
    int mode, int enabled, uint32_t now)
{
    if (mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS) return 0;
    if (!enabled || !af->continuous_armed) return 0;
    if (af->armed || af->safety_paused) return 0;
    if (reference_state != SMOOTHAF_MONITOR_REF_PROVISIONAL) return 0;
    if (af->session_acquisitions >= SMOOTHAF_VLOG_AF_MAX_SESSION_ACQUISITIONS)
        return 0;
    if (af->acquisition_epoch == reference_epoch &&
        af->acquisition_attempts >= SMOOTHAF_VLOG_AF_MAX_ACQUISITION_ATTEMPTS)
        return 0;
    if (af->cooldown_next_episode &&
        (int32_t)(now - af->cooldown_until_ms) < 0) return 0;
    if (af->wait_for_blur_clear &&
        (int32_t)(now - af->blur_recheck_until_ms) < 0) return 0;
    return 1;
}

static inline void smoothaf_vlog_af_begin_acquisition(
    struct smoothaf_vlog_af *af, int reference_epoch)
{
    if (af->acquisition_epoch != reference_epoch)
        af->acquisition_attempts = 0;
    af->acquisition_epoch = reference_epoch;
    if (af->acquisition_attempts != INT32_MAX) af->acquisition_attempts++;
    if (af->session_acquisitions != INT32_MAX) af->session_acquisitions++;
    af->acquisition_attempt_active = 1;
}

static inline void smoothaf_vlog_af_note_acquisition_failure(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    uint32_t retry_at = now + SMOOTHAF_VLOG_AF_ACQUIRE_BACKOFF_MS;
    af->acquisition_attempt_active = 0;
    if (af->cooldown_until_ms < retry_at) af->cooldown_until_ms = retry_at;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
}

static inline int smoothaf_vlog_af_acquisition_exhausted(
    const struct smoothaf_vlog_af *af, int reference_state, int reference_epoch)
{
    return reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL &&
           ((af->acquisition_epoch == reference_epoch &&
             af->acquisition_attempts >= SMOOTHAF_VLOG_AF_MAX_ACQUISITION_ATTEMPTS) ||
            af->session_acquisitions >= SMOOTHAF_VLOG_AF_MAX_SESSION_ACQUISITIONS);
}

/* Manual ACTIVE/DRY RUN arming is itself the authorization for one bounded
 * acquisition; the normal command budget still limits every physical move. */
static inline int smoothaf_vlog_af_should_probe_reference(
    const struct smoothaf_vlog_af *af, int monitor_state, int reference_state,
    int mode)
{
    if (mode == SMOOTHAF_VLOG_AF_OFF || !af->armed ||
        af->state != SMOOTHAF_VLOG_AF_WAITING)
        return 0;
    if (monitor_state == SMOOTHAF_MONITOR_POSSIBLE_BLUR) return 1;
    return reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL;
}

static inline int smoothaf_vlog_af_next_episode_block_reason_for(
    const struct smoothaf_vlog_af *af, int monitor_state, int reference_state,
    int reference_epoch, int mode, int enabled, uint32_t now)
{
    if (mode != SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_MODE;
    if (!enabled) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISABLED;
    if (!af->continuous_armed) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_DISARMED;
    if (af->safety_paused) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_SAFETY_PAUSED;
    if (af->armed) return SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACTIVE;
    if (reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL)
    {
        if (smoothaf_vlog_af_acquisition_exhausted(
                af, reference_state, reference_epoch))
            return SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_EXHAUSTED;
        if (af->acquisition_epoch == reference_epoch)
            return SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_LIMIT;
    }
    else if (reference_state != SMOOTHAF_MONITOR_REF_VALID)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_REFERENCE;
    else if (monitor_state != SMOOTHAF_MONITOR_POSSIBLE_BLUR)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_MONITOR;
    if (af->cooldown_next_episode &&
        (int32_t)(now - af->cooldown_until_ms) < 0)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_COOLDOWN;
    if (af->wait_for_blur_clear &&
        (int32_t)(now - af->blur_recheck_until_ms) < 0)
        return SMOOTHAF_VLOG_AF_NEXT_BLOCK_BLUR_RECHECK;
    return SMOOTHAF_VLOG_AF_NEXT_BLOCK_NONE;
}

static inline int smoothaf_vlog_af_next_episode_block_reason(
    const struct smoothaf_vlog_af *af, int monitor_state, int mode, int enabled,
    uint32_t now)
{
    return smoothaf_vlog_af_next_episode_block_reason_for(
        af, monitor_state, SMOOTHAF_MONITOR_REF_VALID, 0, mode, enabled, now);
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
    int reference_state;
    int reference_reason;
    int reference_epoch;
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
    af->auto_probe_done = 0;
    af->auto_probe_result = SMOOTHAF_AUTO_INVALID;
    af->auto_probe_commands = 0;
    af->auto_probe_reversals = 0;
    af->auto_probe_phase = 0;
    af->auto_probe_first_direction = -1;
    af->auto_probe_selected_direction = 0;
    af->auto_probe_first_score = 0;
    af->auto_probe_opposite_score = 0;
    af->auto_probe_first_delta = 0;
    af->auto_probe_opposite_delta = 0;
    af->auto_probe_enabled = 0;
    af->auto_probe_min_mean = 0;
    af->auto_probe_max_mean = 0;
    af->auto_probe_lighting_valid = 0;
    af->auto_probe_lighting_observed = 0;
    af->algorithm_commands = 0;
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
    af->algorithm_command_limit = SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->consecutive_failures = 0;
    af->neutral_limit_streak = 0;
    af->safety_paused = 0;
    af->continuous_armed = 0;
    af->episodes_completed = 0;
    af->wait_for_blur_clear = 0;
    af->cooldown_next_episode = 0;
    af->command_started = 0;
    af->awaiting_result = 0;
    af->acquisition_epoch = -1;
    af->acquisition_attempts = 0;
    af->session_acquisitions = 0;
    af->acquisition_attempt_active = 0;
    af->recovery_reserved_commands = 0;
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
    af->deadline_ms = 0;
    af->uncertain = 0;
    af->image_ms = 0;
    af->wait_ms = 0;
    af->movement_ms = 0;
    af->stabilization_ms = 0;
    af->total_lens_ms = 0;
    af->total_settle_ms = 0;
    af->total_sample_wait_ms = 0;
    af->lens_calls = 0;
    af->last_result_ms = 0;
    af->last_lens_start_ms = 0;
    af->last_lens_end_ms = 0;
    af->last_settle_start_ms = 0;
    af->last_sample_ms = 0;
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
    int limit = af->episode_limit;
    uint32_t algorithm_limit = af->algorithm_command_limit;
    int auto_probe = af->auto_probe_enabled;
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
    af->episode_limit = limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT &&
                        limit <= SMOOTHAF_VLOG_AF_MAX_COMMANDS
                      ? limit : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->algorithm_command_limit = algorithm_limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT
                                ? algorithm_limit
                                : (uint32_t)af->episode_limit;
    af->auto_probe_enabled = auto_probe;
    af->consecutive_failures = 0;
    af->neutral_limit_streak = 0;
    af->safety_paused = 0;
    af->episodes_completed = completed;
    af->wait_for_blur_clear = 0;
    af->cooldown_next_episode = 0;
    af->command_started = 0;
    af->awaiting_result = 0;
    af->acquisition_epoch = -1;
    af->acquisition_attempts = 0;
    af->session_acquisitions = 0;
    af->acquisition_attempt_active = 0;
    af->recovery_reserved_commands = 0;
    af->dry_run_requests = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    af->controller_initialized = 0;
    af->controller_pending = 0;
    af->controller_step = 1;
    af->command_step = 1;
    af->auto_probe_done = 0;
    af->auto_probe_result = SMOOTHAF_AUTO_INVALID;
    af->auto_probe_commands = 0;
    af->auto_probe_reversals = 0;
    af->auto_probe_phase = 0;
    af->auto_probe_first_direction = -1;
    af->auto_probe_selected_direction = 0;
    af->auto_probe_first_score = 0;
    af->auto_probe_opposite_score = 0;
    af->auto_probe_first_delta = 0;
    af->auto_probe_opposite_delta = 0;
    af->auto_probe_enabled = 0;
    af->auto_probe_min_mean = 0;
    af->auto_probe_max_mean = 0;
    af->auto_probe_lighting_valid = 0;
    af->auto_probe_lighting_observed = 0;
    af->algorithm_commands = 0;
    af->initial_score = baseline;
    af->previous_score = baseline;
    af->post_move_score = 0;
    af->score_delta = 0;
    af->best_position = 0;
    af->current_position = 0;
    af->controller_terminal_result = 0;
    af->last_direction = af->direction;
    af->started_ms = 0;
    af->deadline_ms = 0;
    af->uncertain = 0;
    af->image_ms = 0;
    af->wait_ms = 0;
    af->movement_ms = 0;
    af->stabilization_ms = 0;
    af->total_lens_ms = 0;
    af->total_settle_ms = 0;
    af->total_sample_wait_ms = 0;
    af->lens_calls = 0;
    af->last_result_ms = 0;
    af->last_lens_start_ms = 0;
    af->last_lens_end_ms = 0;
    af->last_settle_start_ms = 0;
    af->last_sample_ms = 0;
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
    af->algorithm_command_limit = (uint32_t)af->episode_limit;
}

static inline int smoothaf_vlog_af_controller_result(int controller_action,
                                                      int result_valid,
                                                      int confirmed)
{
    if (!result_valid) return SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
    if (controller_action == SMOOTHAF_HC_LOCKED ||
        controller_action == SMOOTHAF_ADAPTIVE_LOCKED ||
        controller_action == SMOOTHAF_NORMAL_LOCKED)
        return confirmed ? SMOOTHAF_VLOG_AF_RESULT_CONVERGED
                         : SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
    if (controller_action == SMOOTHAF_HC_NO_CONFIDENCE ||
        controller_action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE ||
        controller_action == SMOOTHAF_NORMAL_NO_CONFIDENCE)
        return SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
    return SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
}

static inline void smoothaf_vlog_af_note_confirmed_success(
    struct smoothaf_vlog_af *af)
{
    af->consecutive_failures = 0;
    af->neutral_limit_streak = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_CONVERGED;
}

static inline void smoothaf_vlog_af_note_neutral_limit(
    struct smoothaf_vlog_af *af)
{
    if (af->neutral_limit_streak != INT32_MAX)
        af->neutral_limit_streak++;
    af->consecutive_failures = 0;
}

static inline void smoothaf_vlog_af_start_continuous_episode(
    struct smoothaf_vlog_af *af, int direction, int baseline)
{
    uint32_t completed = af->episodes_completed;
    int continuous = af->continuous_armed;
    int algorithm = af->algorithm;
    int auto_probe = af->auto_probe_enabled;
    int failures = af->consecutive_failures;
    int neutral_limits = af->neutral_limit_streak;
    int safety_paused = af->safety_paused;
    int limit = af->episode_limit;
    int acquisition_epoch = af->acquisition_epoch;
    int acquisition_attempts = af->acquisition_attempts;
    int session_acquisitions = af->session_acquisitions;
    uint32_t algorithm_limit = af->algorithm_command_limit;
    smoothaf_vlog_af_new_episode(af, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS,
                                 direction, baseline);
    af->continuous_armed = continuous;
    af->episodes_completed = completed;
    af->consecutive_failures = failures;
    af->neutral_limit_streak = neutral_limits;
    af->safety_paused = safety_paused;
    af->acquisition_epoch = acquisition_epoch;
    af->acquisition_attempts = acquisition_attempts;
    af->session_acquisitions = session_acquisitions;
    af->acquisition_attempt_active = 0;
    af->recovery_reserved_commands = 0;
    af->auto_probe_enabled = auto_probe;
    af->algorithm_command_limit = algorithm_limit;
    af->episode_limit = limit >= SMOOTHAF_VLOG_AF_MIN_EPISODE_LIMIT &&
                        limit <= SMOOTHAF_VLOG_AF_MAX_COMMANDS
                      ? limit : SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT;
    af->algorithm = smoothaf_algorithm_normalize(algorithm);
}

static inline void smoothaf_vlog_af_complete_continuous_episode(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    uint32_t cooldown = SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->armed = 0;
    af->awaiting_result = 0;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
    if (af->neutral_limit_streak >= SMOOTHAF_VLOG_AF_MAX_NEUTRAL_LIMIT_STREAK)
        cooldown = SMOOTHAF_VLOG_AF_NEUTRAL_BACKOFF_MS;
    if (af->cooldown_until_ms < now + cooldown)
        af->cooldown_until_ms = now + cooldown;
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

static inline void smoothaf_vlog_af_begin_deadline(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    if (!af->command_started)
    {
        af->started_ms = now;
        af->deadline_ms = now + SMOOTHAF_VLOG_AF_TIMEOUT_MS;
        af->command_started = 1;
    }
}

static inline void smoothaf_vlog_af_record_command(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    smoothaf_vlog_af_begin_deadline(af, now);
    if (af->commands != UINT32_MAX) af->commands++;
    if (af->session_commands != UINT32_MAX) af->session_commands++;
    af->last_command_ms = now;
    if ((!af->auto_probe_enabled || af->auto_probe_done) &&
        af->recovery.phase == SMOOTHAF_RECOVERY_IDLE &&
        af->algorithm_commands != UINT32_MAX)
        af->algorithm_commands++;
    af->awaiting_result = 1;
    if (af->wait_ms <= UINT32_MAX - SMOOTHAF_VLOG_AF_SETTLE_MS)
        af->wait_ms += SMOOTHAF_VLOG_AF_SETTLE_MS;
    af->state = SMOOTHAF_VLOG_AF_SETTLING;
    af->settle_until_ms = now + SMOOTHAF_VLOG_AF_SETTLE_MS;
    af->last_settle_start_ms = now;
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
    if (af->command_started && (int32_t)(now - af->deadline_ms) >= 0)
        return SMOOTHAF_VLOG_AF_BLOCK_DEADLINE;
    if (af->commands >= smoothaf_vlog_af_episode_command_limit(af))
        return SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT;
    if (af->auto_probe_done &&
        smoothaf_vlog_af_algorithm_commands_available(af) == 0)
        return SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT;
    if (!(af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed) &&
        af->session_commands >= smoothaf_vlog_af_session_command_limit(af))
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
    uint32_t limit = smoothaf_vlog_af_algorithm_commands_available(af);
    uint32_t probe_limit = smoothaf_vlog_af_episode_command_limit(af);
    uint32_t episode = af->commands < probe_limit
                     ? probe_limit - af->commands : 0;
    if (af->auto_probe_enabled && af->auto_probe_done) episode = limit;
    uint32_t session;
    if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
        return episode;
    {
        uint32_t session_limit = smoothaf_vlog_af_session_command_limit(af);
        session = session_limit -
                  (af->session_commands < session_limit
                   ? af->session_commands : session_limit);
    }
    return episode < session ? episode : session;
}

/* Overall episode timeout, checked between monitor samples.  An armed episode
 * that has not issued a command yet is not timed out. */
static inline int smoothaf_vlog_af_timeout_due(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    return af->armed && af->command_started &&
           (int32_t)(now - af->deadline_ms) >= 0;
}

static inline void smoothaf_vlog_af_pause_continuous(
    struct smoothaf_vlog_af *af, int result, uint32_t now)
{
    af->armed = 0;
    af->awaiting_result = 0;
    af->episode_result = result;
    af->acquisition_attempt_active = 0;
    if (result == SMOOTHAF_VLOG_AF_RESULT_CANCELLED)
    {
        af->continuous_armed = 0;
        af->state = SMOOTHAF_VLOG_AF_COMPLETE;
        af->cooldown_next_episode = 0;
        af->wait_for_blur_clear = 0;
        smoothaf_recovery_init(&af->recovery);
        af->recovery_reserved_commands = 0;
        return;
    }
    if (af->consecutive_failures != INT32_MAX) af->consecutive_failures++;
    af->state = SMOOTHAF_VLOG_AF_ERROR;
    if (af->cooldown_until_ms < now + SMOOTHAF_VLOG_AF_COOLDOWN_MS)
        af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
    if (af->consecutive_failures >= SMOOTHAF_VLOG_AF_MAX_CONSECUTIVE_FAILURES)
        af->safety_paused = 1;
}

/* A lighting change during the post-move measurement is an environmental
 * event, not evidence that the STM command failed.  Finish the episode without
 * increasing AF Fail Streak or latching SAFETY_PAUSED. */
static inline void smoothaf_vlog_af_abort_environmental(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    af->armed = 0;
    af->awaiting_result = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
    af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
    af->blur_recheck_until_ms = now + SMOOTHAF_VLOG_AF_BLUR_RECHECK_MS;
    af->cooldown_next_episode = 1;
    af->wait_for_blur_clear = 1;
    if (af->episodes_completed != UINT32_MAX) af->episodes_completed++;
    smoothaf_recovery_init(&af->recovery);
}

/* Only a completed search whose final confident sample still represents the
 * measured best may promote a provisional reference.  In particular,
 * NO_IMPROVEMENT is not focus evidence by itself: AUTO must have selected a
 * real side, the controller must have taken a post-probe measurement, and a
 * meaningful improvement must have been observed. */
static inline int smoothaf_vlog_af_result_qualifies_reference(
    int result, int auto_probe_done, int auto_probe_result,
    int algorithm_commands, int best, int initial_score, int score,
    int score_valid, int lighting_valid)
{
    int best_deadband = smoothaf_vlog_af_deadband(best);
    int initial_deadband = smoothaf_vlog_af_deadband(initial_score);
    int meaningful_best = best > initial_score + initial_deadband;
    int final_represents_best = score + best_deadband >= best;

    if (!score_valid || !lighting_valid || !final_represents_best)
        return 0;
    if (result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED)
        return meaningful_best;
    if (result == SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED)
        return meaningful_best && final_represents_best;
    /* NO_IMPROVEMENT requires: AUTO completed with a definitive direction
     * (not UNCERTAIN), at least one algorithm command executed (post-probe
     * measurement), meaningful best score, and the final sample represents
     * the best. A flat terminal result is deliberately never reference
     * evidence. */
    if (result == SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT)
    {
        if (!auto_probe_done)
            return 0;
        if (auto_probe_result == SMOOTHAF_AUTO_UNCERTAIN)
            return 0;
        if (algorithm_commands < 1)
            return 0;
        return meaningful_best && final_represents_best;
    }
    return 0;
}

static inline uint32_t smoothaf_vlog_af_timeout_remaining(
    const struct smoothaf_vlog_af *af, uint32_t now)
{
    uint32_t elapsed;
    if (!af->armed) return 0;
    if (!af->command_started) return SMOOTHAF_VLOG_AF_TIMEOUT_MS;
    if ((int32_t)(now - af->deadline_ms) >= 0) return 0;
    elapsed = af->deadline_ms - now;
    return elapsed;
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
    int limit = (int)smoothaf_vlog_af_algorithm_command_limit(af);
    af->post_move_score = score;
    af->last_result_ms = now;
    af->score_delta = delta;
    af->awaiting_result = 0;
    if (score > af->best)
    {
        af->best = score;
        af->best_position = af->current_position;
    }
    if (delta > deadband)
    {
        af->neutral_limit_streak = 0;
        af->previous_score = score;
        af->episode_result = SMOOTHAF_VLOG_AF_RESULT_IMPROVED;
        af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_COOLDOWN_MS;
        af->state = SMOOTHAF_VLOG_AF_COOLDOWN;
        if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS && af->continuous_armed)
        {
            /* Improvement is not convergence.  Keep the continuous consent
             * alive, but finish this bounded episode and wait for fresh blur
             * evidence before starting another one. */
            if (smoothaf_vlog_af_algorithm_commands_used(af) >= (uint32_t)limit)
                af->episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
            else
                af->episode_result = SMOOTHAF_VLOG_AF_RESULT_IMPROVED;
            smoothaf_vlog_af_complete_continuous_episode(af, now);
            return 1;
        }
        if (smoothaf_vlog_af_algorithm_commands_used(af) >= (uint32_t)limit)
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
        if (smoothaf_vlog_af_algorithm_commands_used(af) >= (uint32_t)limit)
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
        smoothaf_vlog_af_note_neutral_limit(af);
        if (af->neutral_limit_streak >= SMOOTHAF_VLOG_AF_MAX_NEUTRAL_LIMIT_STREAK)
            af->cooldown_until_ms = now + SMOOTHAF_VLOG_AF_NEUTRAL_BACKOFF_MS;
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
    m->reference_state = SMOOTHAF_MONITOR_REF_RECALIBRATE;
    m->reference_reason = 0;
    m->reference_epoch = 0;
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

static inline int smoothaf_monitor_lighting_changed(
    const struct smoothaf_monitor *m, int mean)
{
    int delta;
    if (!m->have_reference) return 0;
    delta = mean - m->reference_mean;
    if (delta < 0) delta = -delta;
    return delta >= SMOOTHAF_MONITOR_LIGHT_DELTA;
}

static inline void smoothaf_monitor_invalidate_reference(
    struct smoothaf_monitor *m)
{
    m->reference_state = SMOOTHAF_MONITOR_REF_RECALIBRATE;
    m->reference_reason = SMOOTHAF_MONITOR_REASON_RECALIBRATE;
    m->have_reference = 0;
    m->possible_blur = 0;
    m->blur_bad_count = 0;
    m->recovery_count = 0;
    m->lighting_count = 0;
}

static inline void smoothaf_monitor_set_reference(
    struct smoothaf_monitor *m, const struct smoothaf_monitor_sample *s,
    int state, int reason, uint32_t now)
{
    m->metric = s->metric;
    m->reference = s->score;
    m->reference_mean = s->mean;
    m->smoothed = s->score;
    m->have_reference = 1;
    m->reference_state = state;
    m->reference_reason = reason;
    m->blur_bad_count = 0;
    m->recovery_count = 0;
    m->lighting_count = 0;
    m->possible_blur = 0;
    if (state == SMOOTHAF_MONITOR_REF_PROVISIONAL &&
        m->reference_epoch != INT32_MAX)
        m->reference_epoch++;
    smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);
    smoothaf_monitor_schedule_after(m, now);
}

/* A completed, bounded AF search is the explicit evidence that promotes a
 * provisional baseline to a reference eligible for blur decisions. */
static inline void smoothaf_monitor_qualify_reference(
    struct smoothaf_monitor *m, int score, int mean, int metric, uint32_t now)
{
    m->metric = metric;
    m->reference = score;
    m->reference_mean = mean;
    m->smoothed = score;
    m->have_reference = 1;
    m->reference_state = SMOOTHAF_MONITOR_REF_VALID;
    m->reference_reason = SMOOTHAF_MONITOR_REASON_AF_RESULT;
    m->blur_bad_count = 0;
    m->recovery_count = 0;
    m->lighting_count = 0;
    m->possible_blur = 0;
    smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);
    smoothaf_monitor_schedule_after(m, now);
}

static inline void smoothaf_monitor_accept(struct smoothaf_monitor *m,
                                           const struct smoothaf_monitor_sample *s,
                                           uint32_t now)
{
    int lighting;
    int low;
    int transient_lighting = 0;
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
        /* An unconfident sample is not evidence in either direction.  It must
         * not extend a blur or recovery run, otherwise low / uncertain / low
         * would count as two genuinely consecutive samples. */
        m->blur_bad_count = 0;
        m->recovery_count = 0;
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    if (!m->have_reference || m->metric != s->metric)
    {
        int reason = m->have_reference
                   ? SMOOTHAF_MONITOR_REASON_METRIC
                   : SMOOTHAF_MONITOR_REASON_INITIAL;
        smoothaf_monitor_set_reference(m, s, SMOOTHAF_MONITOR_REF_PROVISIONAL,
                                       reason, now);
        return;
    }

    lighting = s->mean - m->reference_mean;
    if (lighting < 0) lighting = -lighting;

    if (m->reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL)
    {
        /* A provisional baseline is a placeholder, not focus evidence.  A
         * sustained exposure change opens a new acquisition epoch; a single
         * flicker sample does not. */
        if (lighting >= SMOOTHAF_MONITOR_LIGHT_DELTA)
        {
            m->lighting_count++;
            if (m->lighting_count >= SMOOTHAF_MONITOR_REF_RECALIBRATE_SAMPLES)
            {
                m->reference = s->score;
                m->reference_mean = s->mean;
                m->smoothed = s->score;
                m->reference_reason = SMOOTHAF_MONITOR_REASON_LIGHT;
                m->lighting_count = 0;
                if (m->reference_epoch != INT32_MAX) m->reference_epoch++;
            }
            m->possible_blur = 0;
            m->blur_bad_count = 0;
            m->recovery_count = 0;
            smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        }
        else
        {
            m->smoothed = (m->smoothed * 3 + s->score) / 4;
            m->lighting_count = 0;
            m->possible_blur = 0;
            m->blur_bad_count = 0;
            m->recovery_count = 0;
            smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);
        }
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    if (m->reference_state == SMOOTHAF_MONITOR_REF_RECALIBRATE)
    {
        smoothaf_monitor_set_reference(m, s, SMOOTHAF_MONITOR_REF_PROVISIONAL,
                                       SMOOTHAF_MONITOR_REASON_PROVISIONAL_RECAL,
                                       now);
        return;
    }

    if (lighting >= SMOOTHAF_MONITOR_LIGHT_DELTA)
    {
        m->lighting_count++;
        if (m->reference_state == SMOOTHAF_MONITOR_REF_OBSOLETE ||
            m->lighting_count >= SMOOTHAF_MONITOR_REF_RECALIBRATE_SAMPLES)
        {
            smoothaf_monitor_invalidate_reference(m);
        }
        else if (m->lighting_count >= SMOOTHAF_MONITOR_CONFIRM_SAMPLES)
        {
            m->reference_state = SMOOTHAF_MONITOR_REF_OBSOLETE;
            m->reference_reason = SMOOTHAF_MONITOR_REASON_LIGHT;
            m->possible_blur = 0;
            m->blur_bad_count = 0;
            m->recovery_count = 0;
        }
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }

    if (m->reference_state == SMOOTHAF_MONITOR_REF_OBSOLETE)
    {
        if (lighting > SMOOTHAF_MONITOR_LIGHT_EXIT_DELTA)
        {
            m->lighting_count++;
            if (m->lighting_count >= SMOOTHAF_MONITOR_REF_RECALIBRATE_SAMPLES)
                smoothaf_monitor_invalidate_reference(m);
            smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
            smoothaf_monitor_schedule_after(m, now);
            return;
        }
        /* A short excursion that returns to the original exposure does not
         * invalidate the reference or force a recalibration. */
        m->reference_state = SMOOTHAF_MONITOR_REF_VALID;
        m->lighting_count = 0;
        m->possible_blur = 0;
        m->blur_bad_count = 0;
        m->recovery_count = 0;
        transient_lighting = 1;
    }
    else if (lighting > SMOOTHAF_MONITOR_LIGHT_EXIT_DELTA)
    {
        m->lighting_count++;
        if (m->lighting_count >= SMOOTHAF_MONITOR_REF_RECALIBRATE_SAMPLES)
            smoothaf_monitor_invalidate_reference(m);
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_UNCERTAIN);
        smoothaf_monitor_schedule_after(m, now);
        return;
    }
    else
    {
        if (m->lighting_count) transient_lighting = 1;
        m->lighting_count = 0;
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
        /* The middle band breaks consecutiveness: low / middle / low must not
         * accumulate two blur confirmations. */
        m->recovery_count = 0;
        m->blur_bad_count = 0;
    }

    if (transient_lighting && !m->possible_blur)
        smoothaf_monitor_transition(m, SMOOTHAF_MONITOR_STABLE);

    if (!m->possible_blur && m->state == SMOOTHAF_MONITOR_STABLE &&
        s->score >= m->reference)
    {
        int delta = (s->score - m->reference) / 16;
        if (delta > 0) m->reference += delta;
    }
    smoothaf_monitor_schedule_after(m, now);
}

#endif
