#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/smoothaf_episode.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

struct sim_trace
{
    uint32_t at_ms;
    int direction;
    int step;
    int accepted;
    int before;
    int after;
    int score;
};

struct episode_sim
{
    struct smoothaf_vlog_af af;
    uint32_t now;
    int position;
    int target;
    int direction_setting;
    int lens_ms;
    int settle_ms;
    int lens_calls;
    int fail_call;
    int cancel_before_command;
    int cancel_during_lens;
    int cancel_during_settle;
    int cancel_before_result;
    int lighting_changed;
    int subject_motion;
    int cancelled;
    struct sim_trace trace[SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS];
    int trace_count;
};

static int score_at(int position, int target)
{
    int distance = position - target;
    return 1000 - distance * distance * 100;
}

static int sim_score(const struct episode_sim *sim)
{
    int target = sim->target + (sim->subject_motion ? 1 : 0);
    return score_at(sim->position, target);
}

static void sim_init(struct episode_sim *sim, int algorithm, int target,
                     int direction, int limit)
{
    memset(sim, 0, sizeof(*sim));
    smoothaf_vlog_af_init(&sim->af);
    smoothaf_vlog_af_set_algorithm(&sim->af, algorithm);
    smoothaf_vlog_af_set_episode_limit(&sim->af, limit);
    sim->target = target;
    sim->direction_setting = SMOOTHAF_VLOG_DIRECTION_A;
    sim->lens_ms = 5;
    sim->settle_ms = SMOOTHAF_VLOG_AF_SETTLE_MS;
    sim->now = 100;
    smoothaf_vlog_af_new_episode(&sim->af, SMOOTHAF_VLOG_AF_ACTIVE,
                                 direction, sim_score(sim));
    sim->af.state = SMOOTHAF_VLOG_AF_PROBING;
}

static void sim_record_trace(struct episode_sim *sim, int direction, int step,
                             int accepted, int before, int after, int score)
{
    struct sim_trace *trace;
    if (sim->trace_count >= SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS) return;
    trace = &sim->trace[sim->trace_count++];
    trace->at_ms = sim->now;
    trace->direction = direction;
    trace->step = step;
    trace->accepted = accepted;
    trace->before = before;
    trace->after = after;
    trace->score = score;
}

static int sim_lens_move(struct episode_sim *sim, int direction, int step)
{
    int before = sim->position;
    int accepted;
    sim->lens_calls++;
    accepted = sim->fail_call > 0 && sim->lens_calls == sim->fail_call ? 0 : 1;
    sim->now += (uint32_t)sim->lens_ms;
    if (sim->cancel_during_lens) sim->cancelled = 1;
    if (accepted) sim->position += direction * step;
    sim_record_trace(sim, direction, step, accepted, before, sim->position,
                     accepted ? sim_score(sim) : -1);
    return accepted;
}

static int sim_cancelled(const struct episode_sim *sim)
{
    return sim->cancelled;
}

static void sim_finish(struct episode_sim *sim, int result)
{
    sim->af.episode_result = result;
    sim->af.armed = 0;
    sim->af.awaiting_result = 0;
    sim->af.state = SMOOTHAF_VLOG_AF_COMPLETE;
}

/* This is deliberately the same production ordering: gate, ask the shared
 * seam, start the fixed deadline immediately before the physical call, commit
 * only after acceptance, settle, then feed the post-move score. */
static int sim_run_controller(struct episode_sim *sim)
{
    int direction;
    int step;
    int action;
    int score;
    int auto_measurement;
    int guard = 0;

    while (sim->af.armed && guard++ < 200)
    {
        if (sim_cancelled(sim))
        {
            sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_CANCELLED);
            break;
        }
        if (smoothaf_vlog_af_timeout_due(&sim->af, sim->now))
        {
            sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_TIMEOUT);
            break;
        }
        if (sim->af.awaiting_result)
        {
            if ((int32_t)(sim->af.settle_until_ms - sim->now) > 0)
            {
                sim->now = sim->af.settle_until_ms;
                if (sim->cancel_during_settle) sim->cancelled = 1;
            }
            if (sim->cancel_before_result) sim->cancelled = 1;
            if (sim_cancelled(sim))
            {
                sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_CANCELLED);
                break;
            }
            score = sim_score(sim);
            if (sim->lighting_changed)
            {
                sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN);
                break;
            }
            auto_measurement = sim->af.auto_probe_enabled &&
                                !sim->af.auto_probe_done;
            smoothaf_episode_prepare_score(&sim->af, score, sim->now);
            if (auto_measurement)
            {
                smoothaf_vlog_af_auto_observe_mean(&sim->af, 100);
                action = smoothaf_vlog_af_auto_on_score(&sim->af, score);
                (void)action;
                sim->af.episode_result = SMOOTHAF_VLOG_AF_RESULT_IMPROVED;
                sim->af.state = SMOOTHAF_VLOG_AF_PROBING;
            }
            else
            {
                action = smoothaf_episode_on_score(&sim->af, score);
                if (action == 2)
                {
                    smoothaf_vlog_af_record_measure(&sim->af, sim->now);
                }
                else if (!action)
                {
                    int result = smoothaf_vlog_af_controller_result(
                        sim->af.controller_action, 1, 1);
                    sim_finish(sim, result);
                }
            }
            continue;
        }
        if (sim->cancel_before_command)
        {
            sim->cancelled = 1;
            continue;
        }
        if (smoothaf_vlog_af_command_block_reason(&sim->af, 1, sim->now) !=
            SMOOTHAF_VLOG_AF_BLOCK_NONE)
        {
            sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT);
            break;
        }
        if (!smoothaf_episode_next_command(&sim->af,
                                           sim->direction_setting, 1, 1,
                                           &direction, &step))
        {
            int result = smoothaf_vlog_af_controller_result(
                sim->af.controller_action, 1, 1);
            sim_finish(sim, result);
            break;
        }
        smoothaf_vlog_af_begin_deadline(&sim->af, sim->now);
        if (!sim_lens_move(sim, direction, step))
        {
            smoothaf_episode_rollback_move(&sim->af, direction, step);
            if (sim_cancelled(sim))
                sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_CANCELLED);
            else
                sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_FAILED);
            break;
        }
        /* A movement accepted by the lens is committed even if cancellation
         * arrived during that call. */
        smoothaf_episode_commit_move(&sim->af, direction, step, sim->now);
        sim->af.current_position = sim->position;
        if (sim_cancelled(sim))
        {
            sim_finish(sim, SMOOTHAF_VLOG_AF_RESULT_CANCELLED);
            break;
        }
    }
    CHECK(guard < 200, "shared seam simulator did not terminate");
    return sim->af.episode_result;
}

static void test_legacy_peak_and_reversal(void)
{
    struct episode_sim sim;
    int result;
    sim_init(&sim, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 12);
    result = sim_run_controller(&sim);
    CHECK(result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED,
          "Legacy HC valid LOCKED result is CONVERGED");
    CHECK(sim.af.reversals == 1, "Legacy HC returned through one reversal");
    CHECK(sim.af.best_position == 2,
          "Legacy HC recorded the representative best position");
    CHECK(sim.af.commands <= 12 && sim.lens_calls == (int)sim.af.commands,
          "accepted physical commands are bounded and accounted");
    CHECK(sim.trace_count > 0 && sim.trace[0].accepted,
          "trace records accepted lens commands");
}

static void test_position_units_and_auto_baseline(void)
{
    struct smoothaf_vlog_af af;
    int direction;
    int step;
    int baseline;

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_set_algorithm(&af, SMOOTHAF_ALGORITHM_LEGACY_HC);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 600);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    baseline = af.initial_score;
    CHECK(smoothaf_episode_next_command(&af, SMOOTHAF_VLOG_DIRECTION_A,
                                        1, 3, &direction, &step),
          "Legacy HC exposes the first step through the shared seam");
    smoothaf_episode_commit_move(&af, direction, step, 100);
    smoothaf_episode_prepare_score(&af, 900, 200);
    CHECK(smoothaf_episode_on_score(&af, 900),
          "Legacy HC schedules the next move after an improvement");
    CHECK(af.best_position == 3,
          "Legacy HC public best position uses lens-step units");

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_set_algorithm(&af, SMOOTHAF_ALGORITHM_LEGACY_HC);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 600);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    smoothaf_vlog_af_set_auto_probe(&af, 1);
    CHECK(smoothaf_vlog_af_auto_needs_move(&af, SMOOTHAF_VLOG_DIRECTION_AUTO,
                                           1, &direction, &step),
          "AUTO exposes its first probe move");
    smoothaf_episode_commit_move(&af, direction, step, 100);
    smoothaf_episode_prepare_score(&af, 900, 200);
    smoothaf_episode_on_auto_score(&af, 900);
    CHECK(af.best == 900,
          "AUTO records its measured peak before the probe completes");
    /* The remaining probe samples return to the baseline and finish AUTO. */
    smoothaf_episode_commit_move(&af, -direction, step, 300);
    smoothaf_episode_prepare_score(&af, 600, 400);
    smoothaf_episode_on_auto_score(&af, 600);
    smoothaf_episode_commit_move(&af, -direction, step, 500);
    smoothaf_episode_prepare_score(&af, 300, 600);
    smoothaf_episode_on_auto_score(&af, 300);
    smoothaf_episode_commit_move(&af, direction, step, 700);
    smoothaf_episode_prepare_score(&af, baseline, 800);
    smoothaf_episode_on_auto_score(&af, baseline);
    CHECK(af.auto_probe_done && af.current_position == 0,
          "AUTO completes at the physical baseline position");
    CHECK(af.best == baseline && af.best_position == 0 &&
          af.previous_score == baseline,
          "AUTO evidence does not contaminate algorithm baseline state");
}

static void test_recovery_position_is_episode_relative(void)
{
    struct smoothaf_vlog_af af;
    int direction;
    int step;

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 100);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    af.current_position = 6;
    smoothaf_episode_begin_recovery(&af, 100, 1);
    smoothaf_vlog_af_reserve_recovery(&af, 1);
    CHECK(smoothaf_recovery_next(&af.recovery, &direction, &step),
          "recovery exposes its first bounded move");
    smoothaf_episode_commit_recovery_move(&af, direction, step, 100);
    CHECK(af.current_position == 9,
          "recovery position remains in the episode coordinate system");
}

static void test_other_controllers_use_same_seam(void)
{
    const int algorithms[] = {
        SMOOTHAF_ALGORITHM_ADAPTIVE_HC,
        SMOOTHAF_ALGORITHM_NORMAL_AF,
    };
    size_t i;
    for (i = 0; i < sizeof(algorithms) / sizeof(algorithms[0]); i++)
    {
        struct episode_sim sim;
        int result;
        sim_init(&sim, algorithms[i], 2, 1, 12);
        result = sim_run_controller(&sim);
        CHECK(result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED ||
              result == SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT,
              "adaptive/normal result is mapped through the shared seam");
        CHECK(sim.af.commands <= 12,
              "adaptive/normal physical episode budget is bounded");
    }
}

static void test_auto_direction_and_budget(void)
{
    struct episode_sim sim;
    int result;
    sim_init(&sim, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 12);
    smoothaf_vlog_af_set_auto_probe(&sim.af, 1);
    sim.direction_setting = SMOOTHAF_VLOG_DIRECTION_AUTO;
    result = sim_run_controller(&sim);
    CHECK(result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED,
          "AUTO episode can still converge only after confirmation");
    CHECK(sim.af.auto_probe_done && sim.af.auto_probe_result ==
          SMOOTHAF_AUTO_FIRST_IMPROVED,
          "AUTO selects the improving direction");
    CHECK(sim.af.auto_probe_commands == SMOOTHAF_AUTO_MAX_COMMANDS,
          "AUTO probe commands are counted separately");
    CHECK(sim.af.commands <= smoothaf_vlog_af_episode_command_limit(&sim.af),
          "AUTO and algorithm commands share one physical episode budget");
}

static void test_failed_move_rolls_back_controller(void)
{
    struct episode_sim sim;
    int before;
    sim_init(&sim, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    sim.fail_call = 1;
    before = sim.af.legacy_controller.commands;
    CHECK(sim_run_controller(&sim) == SMOOTHAF_VLOG_AF_RESULT_FAILED,
          "failed lens call is a real failure");
    CHECK(sim.af.commands == 0 && sim.af.current_position == 0,
          "failed lens call is not committed to episode accounting");
    CHECK(sim.af.legacy_controller.commands == before,
          "failed lens call rolls back controller reservation");
    CHECK(sim.lens_calls == 1 && sim.trace[0].accepted == 0,
          "failure trace preserves the rejected physical call");
}

static void test_cancellation_phases(void)
{
    struct episode_sim before;
    struct episode_sim during;
    struct episode_sim settling;
    struct episode_sim result_phase;

    sim_init(&before, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    before.cancel_before_command = 1;
    CHECK(sim_run_controller(&before) == SMOOTHAF_VLOG_AF_RESULT_CANCELLED,
          "STOP/PANIC before a command cancels without movement");
    CHECK(before.lens_calls == 0 && before.af.commands == 0,
          "pre-command cancellation issues no lens call");

    sim_init(&during, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    during.cancel_during_lens = 1;
    CHECK(sim_run_controller(&during) == SMOOTHAF_VLOG_AF_RESULT_CANCELLED,
          "cancellation during lens call is CANCELLED");
    CHECK(during.lens_calls == 1 && during.af.commands == 1 &&
          during.af.current_position == during.position,
          "accepted movement is committed before cancellation finalization");

    sim_init(&settling, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    settling.cancel_during_settle = 1;
    CHECK(sim_run_controller(&settling) == SMOOTHAF_VLOG_AF_RESULT_CANCELLED,
          "cancellation during settle prevents the post-move sample");
    CHECK(settling.lens_calls == 1 && settling.af.commands == 1,
          "settle cancellation does not issue another command");

    sim_init(&result_phase, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    result_phase.cancel_before_result = 1;
    CHECK(sim_run_controller(&result_phase) == SMOOTHAF_VLOG_AF_RESULT_CANCELLED,
          "cancellation immediately before the post-move result is CANCELLED");
    CHECK(result_phase.lens_calls == 1 && result_phase.af.commands == 1,
          "result-phase cancellation preserves accepted command accounting");
}

static void test_fixed_deadline_and_limits(void)
{
    struct episode_sim sim;
    uint32_t deadline;
    int limits[] = { 4, 6, 8, 12 };
    size_t i;

    sim_init(&sim, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    smoothaf_vlog_af_begin_deadline(&sim.af, 4000);
    deadline = sim.af.deadline_ms;
    sim.now = 4000 + SMOOTHAF_VLOG_AF_TIMEOUT_MS - 1;
    smoothaf_vlog_af_record_command(&sim.af, sim.now);
    CHECK(sim.af.deadline_ms == deadline,
          "later commands cannot extend the fixed deadline");
    sim.now = deadline;
    CHECK(smoothaf_vlog_af_timeout_due(&sim.af, sim.now),
          "deadline is due at the exact boundary");
    sim.now = deadline + 1;
    CHECK(smoothaf_vlog_af_timeout_due(&sim.af, sim.now),
          "deadline remains due after the boundary");

    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
    {
        smoothaf_vlog_af_init(&sim.af);
        smoothaf_vlog_af_set_episode_limit(&sim.af, limits[i]);
        CHECK(smoothaf_vlog_af_algorithm_command_limit(&sim.af) ==
              (uint32_t)limits[i], "configured 4/6/8/12 budget is preserved");
    }
}

static void test_recovery_with_physical_trace(void)
{
    struct episode_sim sim;
    int direction;
    int step;
    int score;
    int guard = 0;

    memset(&sim, 0, sizeof(sim));
    smoothaf_vlog_af_init(&sim.af);
    smoothaf_vlog_af_set_episode_limit(&sim.af, 12);
    smoothaf_vlog_af_new_episode(&sim.af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 100);
    sim.af.state = SMOOTHAF_VLOG_AF_PROBING;
    sim.target = 3;
    smoothaf_recovery_begin(&sim.af.recovery, 100, 1);
    smoothaf_vlog_af_reserve_recovery(&sim.af, 1);

    while (sim.af.recovery.phase != SMOOTHAF_RECOVERY_COMPLETE &&
           sim.af.recovery.phase != SMOOTHAF_RECOVERY_ABORTED && guard++ < 30)
    {
        CHECK(smoothaf_recovery_next(&sim.af.recovery, &direction, &step),
              "recovery requests only bounded physical commands");
        sim.now += 5;
        sim.position = sim.af.recovery.pending_position;
        smoothaf_recovery_commit_move(&sim.af.recovery);
        smoothaf_vlog_af_record_command(&sim.af, sim.now);
        score = sim.position == 3 ? 400 : 100 - abs(sim.position) * 2;
        smoothaf_recovery_on_score(&sim.af.recovery, score);
    }
    CHECK(guard < 30, "recovery simulator terminates");
    CHECK(sim.af.recovery.result == SMOOTHAF_RECOVERY_BEST_RECOVERED,
          "recovery requires a representative score at the returned best");
    CHECK(sim.af.commands <= SMOOTHAF_RECOVERY_MAX_COMMANDS,
          "recovery physical commands remain bounded");
    smoothaf_vlog_af_reserve_recovery(&sim.af, 0);
    CHECK(!sim.af.recovery_reserved_commands,
          "recovery reservation is cleared at terminal cleanup");
}

static void test_lighting_motion_and_dry_run_edges(void)
{
    struct episode_sim lighting;
    struct episode_sim motion;
    struct smoothaf_vlog_af dry;
    int direction;
    int step;
    int before;
    int result;

    sim_init(&lighting, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    lighting.lighting_changed = 1;
    CHECK(sim_run_controller(&lighting) == SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "lighting change is environmental uncertainty, not convergence");
    CHECK(lighting.af.consecutive_failures == 0,
          "lighting uncertainty does not increment the failure streak");

    sim_init(&motion, SMOOTHAF_ALGORITHM_LEGACY_HC, 2, 1, 8);
    motion.subject_motion = 1;
    result = sim_run_controller(&motion);
    CHECK(result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED ||
          result == SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT ||
          result == SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "subject motion remains bounded by the shared episode seam");
    CHECK(motion.af.commands <= 8, "subject motion cannot exceed the budget");

    smoothaf_vlog_af_init(&dry);
    smoothaf_vlog_af_set_episode_limit(&dry, 4);
    smoothaf_vlog_af_new_episode(&dry, SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1000);
    dry.state = SMOOTHAF_VLOG_AF_PROBING;
    before = dry.current_position;
    CHECK(smoothaf_episode_next_command(&dry, SMOOTHAF_VLOG_DIRECTION_A, 1, 1,
                                       &direction, &step),
          "dry-run episode still exposes command intent through the seam");
    CHECK(dry.current_position == before && dry.commands == 0,
          "dry-run host policy never changes the logical lens position");
}

static void test_monitor_policy_edges(void)
{
    struct smoothaf_vlog_af af;
    struct smoothaf_monitor monitor;

    smoothaf_vlog_af_init(&af);
    CHECK(smoothaf_vlog_af_controller_result(SMOOTHAF_HC_LOCKED, 1, 0) ==
          SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "unconfirmed LOCKED is not convergence");
    CHECK(smoothaf_vlog_af_controller_result(SMOOTHAF_HC_LOCKED, 1, 1) ==
          SMOOTHAF_VLOG_AF_RESULT_CONVERGED,
          "confirmed LOCKED is convergence");
    CHECK(smoothaf_vlog_af_controller_result(SMOOTHAF_HC_LOCKED, 0, 1) ==
          SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "invalid LOCKED sample is uncertain");

    smoothaf_monitor_init(&monitor, SMOOTHAF_MONITOR_METRIC_SAD,
                          SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
    smoothaf_monitor_start(&monitor, SMOOTHAF_MONITOR_METRIC_SAD, 0);
    CHECK(monitor.reference_state == SMOOTHAF_MONITOR_REF_RECALIBRATE,
          "monitor starts without a falsely valid reference");
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS,
                                 1, 1000);
    af.continuous_armed = 1;
    af.neutral_limit_streak = SMOOTHAF_VLOG_AF_MAX_NEUTRAL_LIMIT_STREAK;
    smoothaf_vlog_af_complete_continuous_episode(&af, 100);
    CHECK(af.cooldown_until_ms - 100 == SMOOTHAF_VLOG_AF_NEUTRAL_BACKOFF_MS,
          "neutral-limit backoff survives continuous completion");
    smoothaf_vlog_af_pause_continuous(&af, SMOOTHAF_VLOG_AF_RESULT_FAILED, 100);
    CHECK(af.consecutive_failures == 1 && af.continuous_armed,
          "continuous real failure preserves consent while counting failure");
    CHECK(af.continuous_armed && !af.armed,
          "continuous consent remains separate from bounded episode armed state");
}

int main(void)
{
    test_legacy_peak_and_reversal();
    test_position_units_and_auto_baseline();
    test_recovery_position_is_episode_relative();
    test_other_controllers_use_same_seam();
    test_auto_direction_and_budget();
    test_failed_move_rolls_back_controller();
    test_cancellation_phases();
    test_fixed_deadline_and_limits();
    test_recovery_with_physical_trace();
    test_lighting_motion_and_dry_run_edges();
    test_monitor_policy_edges();
    if (failures) {
        printf("SmoothAF episode simulator failures: %d\n", failures);
        return 1;
    }
    printf("SmoothAF episode/controller simulator tests passed (SIMULATION)\n");
    return 0;
}
