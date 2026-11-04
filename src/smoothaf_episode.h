#ifndef SMOOTHAF_EPISODE_H
#define SMOOTHAF_EPISODE_H

#include "smoothaf_monitor.h"

/* Hardware-independent monitor/controller adapter.  The production task owns
 * timing, gates, lens_focus(), measurements and telemetry; this seam owns the
 * controller transition ordering and its accepted/failed-command bookkeeping.
 * Host tests use the same functions instead of reproducing the adapter. */

static inline void smoothaf_episode_controller_init(
    struct smoothaf_vlog_af *af, int step)
{
    struct smoothaf_adaptive_policy policy;
    int algorithm = af->algorithm;
    if (af->controller_initialized) return;
    af->controller_initialized = 1;
    af->controller_step = step;
    af->command_step = step;
    if (algorithm == SMOOTHAF_ALGORITHM_LEGACY_HC)
    {
        smoothaf_hc_init(&af->legacy_controller, af->initial_score,
                         af->direction);
        /* Reserve the first movement during initialization, matching the
         * controller's later on-score reservations. */
        af->controller_pending =
            smoothaf_hc_next_move(&af->legacy_controller) == SMOOTHAF_HC_MOVE;
    }
    else if (algorithm == SMOOTHAF_ALGORITHM_ADAPTIVE_HC)
    {
        smoothaf_adaptive_policy_default(&policy);
        policy.min_step = step;
        policy.initial_step = step;
        policy.max_step = step;
        policy.max_commands = SMOOTHAF_VLOG_AF_MAX_COMMANDS;
        smoothaf_adaptive_hc_init(&af->adaptive_controller, &policy,
                                  af->initial_score, af->direction);
        af->controller_action = smoothaf_adaptive_hc_begin(
            &af->adaptive_controller);
    }
    else if (algorithm == SMOOTHAF_ALGORITHM_NORMAL_AF)
    {
        smoothaf_normal_af_init(&af->normal_controller, af->initial_score,
                                af->direction, step);
        af->controller_action = smoothaf_normal_af_begin(
            &af->normal_controller);
    }
}

static inline void smoothaf_episode_commit_move(
    struct smoothaf_vlog_af *af, int direction, int step, uint32_t now)
{
    af->direction = direction;
    af->command_step = step;
    if (af->auto_probe_enabled && !af->auto_probe_done)
        smoothaf_vlog_af_auto_commit_move(af);
    smoothaf_vlog_af_record_command(af, now);
    af->current_position += direction * step;
}

static inline void smoothaf_episode_rollback_move(
    struct smoothaf_vlog_af *af, int direction, int step)
{
    int algorithm = af->algorithm;
    if (af->auto_probe_enabled && !af->auto_probe_done) return;
    if (algorithm == SMOOTHAF_ALGORITHM_LEGACY_HC)
    {
        /* Legacy HC reserves the next move before lens_focus().  Undo only
         * that reservation; the caller has not committed a physical command. */
        if (af->legacy_controller.commands > 0)
        {
            af->legacy_controller.commands--;
            af->legacy_controller.pos -= af->legacy_controller.dir;
        }
        af->controller_pending = 1;
    }
    else if (algorithm == SMOOTHAF_ALGORITHM_ADAPTIVE_HC)
    {
        if (af->adaptive_controller.commands > 0)
            af->adaptive_controller.commands--;
        af->adaptive_controller.pos -= direction * step;
        af->adaptive_controller.pending = 1;
    }
    else if (algorithm == SMOOTHAF_ALGORITHM_NORMAL_AF)
    {
        if (af->normal_controller.commands > 0)
            af->normal_controller.commands--;
        if (af->normal_controller.phase == SMOOTHAF_NORMAL_PHASE_ACQUISITION)
        {
            if (af->normal_controller.acquisition_commands > 0)
                af->normal_controller.acquisition_commands--;
        }
        else if (af->normal_controller.refinement_commands > 0)
            af->normal_controller.refinement_commands--;
        af->normal_controller.pos -= direction * step;
        af->normal_controller.pending = 1;
    }
}

static inline void smoothaf_episode_rollback_recovery(
    struct smoothaf_vlog_af *af)
{
    if (af->recovery.commands > 0) af->recovery.commands--;
    af->recovery.pending_position = af->recovery.position;
}

static inline int smoothaf_episode_next_command(
    struct smoothaf_vlog_af *af, int direction_setting, int probe_step,
    int controller_step, int *direction, int *step)
{
    int algorithm = af->algorithm;
    if (!af->auto_probe_done &&
        smoothaf_vlog_af_auto_needs_move(af, direction_setting, probe_step,
                                         direction, step))
        return 1;
    smoothaf_episode_controller_init(af, controller_step);
    if (algorithm == SMOOTHAF_ALGORITHM_LEGACY_HC)
    {
        if (!af->controller_pending) return 0;
        af->controller_pending = 0;
        if (direction) *direction = af->legacy_controller.dir;
        if (step) *step = controller_step;
        return 1;
    }
    if (algorithm == SMOOTHAF_ALGORITHM_ADAPTIVE_HC)
    {
        if (af->controller_action != SMOOTHAF_ADAPTIVE_STEP) return 0;
        return smoothaf_adaptive_hc_next_step(&af->adaptive_controller,
                                              direction, step);
    }
    if (algorithm == SMOOTHAF_ALGORITHM_NORMAL_AF)
    {
        if (af->controller_action != SMOOTHAF_NORMAL_STEP) return 0;
        return smoothaf_normal_af_next_step(&af->normal_controller,
                                            direction, step);
    }
    if (direction) *direction = af->direction;
    if (step) *step = controller_step;
    return 1;
}

static inline void smoothaf_episode_prepare_score(
    struct smoothaf_vlog_af *af, int score, uint32_t now)
{
    af->post_move_score = score;
    af->score_delta = score - af->previous_score;
    af->previous_score = score;
    af->last_result_ms = now;
    af->awaiting_result = 0;
    if (score > af->best)
    {
        af->best = score;
        af->best_position = af->current_position;
    }
    af->state = SMOOTHAF_VLOG_AF_PROBING;
}

/* Return 1 for a move, 2 for a confirmation measurement, and 0 for a
 * controller terminal action.  The caller supplies the confirmed flag when it
 * maps the terminal action to CONVERGED. */
static inline int smoothaf_episode_on_score(
    struct smoothaf_vlog_af *af, int score)
{
    int algorithm = af->algorithm;
    af->controller_measure_only = 0;
    if (algorithm == SMOOTHAF_ALGORITHM_LEGACY_HC)
    {
        enum smoothaf_hc_action action = smoothaf_hc_on_score(
            &af->legacy_controller, score);
        af->controller_action = action;
        af->direction = af->legacy_controller.dir;
        af->reversals = af->legacy_controller.reversals;
        af->best = af->legacy_controller.best;
        af->best_position = af->legacy_controller.best_pos;
        af->controller_pending = action == SMOOTHAF_HC_MOVE;
        if (action == SMOOTHAF_HC_MEASURE)
        {
            af->controller_measure_only = 1;
            return 2;
        }
        return action == SMOOTHAF_HC_MOVE;
    }
    if (algorithm == SMOOTHAF_ALGORITHM_ADAPTIVE_HC)
    {
        af->controller_action = smoothaf_adaptive_hc_on_score(
            &af->adaptive_controller, score);
        af->direction = af->adaptive_controller.direction;
        af->reversals = af->adaptive_controller.reversals;
        af->best = af->adaptive_controller.best;
        af->best_position = af->adaptive_controller.best_pos;
        if (af->controller_action == SMOOTHAF_ADAPTIVE_CONFIRM)
        {
            af->controller_measure_only = 1;
            return 2;
        }
        return af->controller_action == SMOOTHAF_ADAPTIVE_STEP;
    }
    if (algorithm == SMOOTHAF_ALGORITHM_NORMAL_AF)
    {
        af->controller_action = smoothaf_normal_af_on_score(
            &af->normal_controller, score);
        af->direction = af->normal_controller.direction;
        af->reversals = af->normal_controller.reversals;
        af->best = af->normal_controller.best;
        af->best_position = af->normal_controller.best_pos;
        if (af->controller_action == SMOOTHAF_NORMAL_CONFIRM)
        {
            af->controller_measure_only = 1;
            return 2;
        }
        return af->controller_action == SMOOTHAF_NORMAL_STEP;
    }
    return 1;
}

#endif
