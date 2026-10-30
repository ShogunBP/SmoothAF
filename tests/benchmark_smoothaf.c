/*
 * Comparative SmoothAF benchmark: Legacy HC versus Adaptive HC.
 *
 * This is a host-only deterministic simulation.  Both controllers consume the
 * same scenario, score function, initial score and injected failures.  The
 * reported ticks are synthetic workload units, not wall-clock or lens time.
 *
 * Build (GCC/Clang):
 *   gcc -std=c99 -Wall -Wextra -O2 -o benchmark_smoothaf tests/benchmark_smoothaf.c
 */

#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_core.h"

#define BENCH_REPETITIONS 100
#define SIM_MOVE_TICKS     5U
#define SIM_MEASURE_TICKS  2U
#define SIM_CONTROL_TICKS  1U

struct bench_scenario
{
    const char *name;
    int initial_score;
    int (*score_at)(int pos);
    const int *sequence;
    int sequence_length;
    int cancel_after_moves;
    int timeout_after_moves;
    int fail_move_at;
    int fail_measure_at;
};

struct sim_state
{
    const struct bench_scenario *scenario;
    int pos;
    int moves;
    int measures;
    int sequence_index;
    int travel;
    int errors;
    unsigned int ticks;
};

struct bench_result
{
    int result;
    int commands;
    int travel;
    int reversals;
    int measures;
    int errors;
    unsigned int ticks;
};

static int abs_i(int value)
{
    return value < 0 ? -value : value;
}

static int peak_at_3(int pos)
{
    return 1000 - 100 * abs_i(pos - 3);
}

static int peak_at_0(int pos)
{
    return 1000 - 100 * abs_i(pos);
}

static int peak_at_minus3(int pos)
{
    return 1000 - 100 * abs_i(pos + 3);
}

static int increasing(int pos)
{
    return 1000 + 100 * pos;
}

static int flat(int pos)
{
    (void)pos;
    return 1000;
}

static int noisy(int pos)
{
    if (pos > 0) return 1010;
    if (pos < 0) return 990;
    return 1000;
}

static int sharp_peak(int pos)
{
    return 1000 - 500 * abs_i(pos - 2);
}

static void sim_reset(struct sim_state *sim, const struct bench_scenario *scenario)
{
    memset(sim, 0, sizeof(*sim));
    sim->scenario = scenario;
}

static int sim_should_abort(const struct sim_state *sim)
{
    if (sim->scenario->cancel_after_moves &&
        sim->moves >= sim->scenario->cancel_after_moves)
        return SMOOTHAF_RESULT_CANCELLED;
    if (sim->scenario->timeout_after_moves &&
        sim->moves >= sim->scenario->timeout_after_moves)
        return SMOOTHAF_RESULT_TIMEOUT;
    return 0;
}

static int sim_move(struct sim_state *sim, int direction, int step)
{
    sim->moves++;
    sim->ticks += SIM_MOVE_TICKS;
    if (sim->scenario->fail_move_at &&
        sim->moves == sim->scenario->fail_move_at)
    {
        sim->errors++;
        return 0;
    }
    sim->pos += direction * step;
    sim->travel += step < 0 ? -step : step;
    return 1;
}

static int sim_measure(struct sim_state *sim, int *score)
{
    sim->measures++;
    sim->ticks += SIM_MEASURE_TICKS;
    if (sim->scenario->fail_measure_at &&
        sim->measures == sim->scenario->fail_measure_at)
    {
        sim->errors++;
        return 0;
    }
    if (sim->scenario->sequence &&
        sim->sequence_index < sim->scenario->sequence_length)
    {
        *score = sim->scenario->sequence[sim->sequence_index++];
    }
    else
    {
        *score = sim->scenario->score_at(sim->pos);
    }
    return 1;
}

static int legacy_move(void *ctx, int direction)
{
    return sim_move((struct sim_state *)ctx, direction, 1);
}

static int legacy_measure(void *ctx, int *score)
{
    return sim_measure((struct sim_state *)ctx, score);
}

static int legacy_abort(void *ctx)
{
    return sim_should_abort((const struct sim_state *)ctx);
}

static int adaptive_abort(const struct sim_state *sim)
{
    return sim_should_abort(sim);
}

static enum smoothaf_result run_legacy_once(
        const struct bench_scenario *scenario, struct bench_result *out)
{
    struct sim_state sim;
    struct smoothaf_hc hc;
    enum smoothaf_result result;

    sim_reset(&sim, scenario);
    sim.ticks += SIM_CONTROL_TICKS;
    result = smoothaf_hc_run(&hc, scenario->initial_score, 1,
                             legacy_move, legacy_measure, legacy_abort, &sim);
    out->result = result;
    out->commands = hc.commands;
    out->travel = sim.travel;
    out->reversals = hc.reversals;
    out->measures = sim.measures + 1; /* include the shared initial observation */
    out->errors = sim.errors;
    out->ticks = sim.ticks;
    return result;
}

static enum smoothaf_result adaptive_result(enum smoothaf_adaptive_action action)
{
    switch (action)
    {
        case SMOOTHAF_ADAPTIVE_LOCKED:        return SMOOTHAF_RESULT_LOCKED;
        case SMOOTHAF_ADAPTIVE_NO_PROGRESS:  return SMOOTHAF_RESULT_NO_CONFIDENCE;
        case SMOOTHAF_ADAPTIVE_NO_CONFIDENCE: return SMOOTHAF_RESULT_NO_CONFIDENCE;
        case SMOOTHAF_ADAPTIVE_LIMIT:        return SMOOTHAF_RESULT_LIMIT;
        default:                             return SMOOTHAF_RESULT_ERROR;
    }
}

static enum smoothaf_result run_adaptive_once(
        const struct bench_scenario *scenario, struct bench_result *out)
{
    struct smoothaf_adaptive_hc hc;
    struct smoothaf_adaptive_policy policy;
    struct sim_state sim;
    enum smoothaf_adaptive_action action;
    int direction;
    int step;
    int score;
    int stop;

    sim_reset(&sim, scenario);
    sim.ticks += SIM_CONTROL_TICKS;
    smoothaf_adaptive_policy_default(&policy);
    smoothaf_adaptive_hc_init(&hc, &policy, scenario->initial_score, 1);
    action = smoothaf_adaptive_hc_begin(&hc);

    if (action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE)
    {
        out->result = SMOOTHAF_RESULT_NO_CONFIDENCE;
    }
    else
    {
        for (;;)
        {
            stop = adaptive_abort(&sim);
            if (stop)
            {
                out->result = (enum smoothaf_result)stop;
                break;
            }
            if (action == SMOOTHAF_ADAPTIVE_STEP)
            {
                if (!smoothaf_adaptive_hc_next_step(&hc, &direction, &step))
                {
                    out->result = SMOOTHAF_RESULT_LIMIT;
                    break;
                }
                if (!sim_move(&sim, direction, step))
                {
                    out->result = SMOOTHAF_RESULT_LENS_ERROR;
                    break;
                }
                stop = adaptive_abort(&sim);
                if (stop)
                {
                    out->result = (enum smoothaf_result)stop;
                    break;
                }
            }
            if (!sim_measure(&sim, &score))
            {
                out->result = SMOOTHAF_RESULT_MEASURE_ERROR;
                break;
            }
            stop = adaptive_abort(&sim);
            if (stop)
            {
                out->result = (enum smoothaf_result)stop;
                break;
            }
            action = smoothaf_adaptive_hc_on_score(&hc, score);
            if (action == SMOOTHAF_ADAPTIVE_LOCKED ||
                action == SMOOTHAF_ADAPTIVE_NO_PROGRESS ||
                action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE ||
                action == SMOOTHAF_ADAPTIVE_LIMIT)
            {
                out->result = adaptive_result(action);
                break;
            }
        }
    }

    out->commands = hc.commands;
    out->travel = sim.travel;
    out->reversals = hc.reversals;
    out->measures = sim.measures + 1;
    out->errors = sim.errors;
    out->ticks = sim.ticks;
    return (enum smoothaf_result)out->result;
}

static const char *result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_RESULT_LOCKED:        return "LOCKED";
        case SMOOTHAF_RESULT_LIMIT:         return "LIMIT";
        case SMOOTHAF_RESULT_NO_CONFIDENCE: return "NO_CONFIDENCE";
        case SMOOTHAF_RESULT_CANCELLED:     return "CANCELLED";
        case SMOOTHAF_RESULT_TIMEOUT:       return "TIMEOUT";
        case SMOOTHAF_RESULT_LENS_ERROR:    return "LENS_ERROR";
        case SMOOTHAF_RESULT_MEASURE_ERROR: return "MEASURE_ERROR";
        default:                            return "ERROR";
    }
}

static int invariant_ok(const struct bench_result *result, int adaptive)
{
    int max_commands = adaptive ? SMOOTHAF_ADAPTIVE_MAX_COMMANDS
                                : SMOOTHAF_MAX_COMMANDS;
    int max_reversals = adaptive ? SMOOTHAF_ADAPTIVE_MAX_REVERSALS
                                 : SMOOTHAF_MAX_REVERSALS;

    return result->commands >= 0 && result->commands <= max_commands &&
           result->travel >= 0 && result->travel <= result->commands *
           (adaptive ? SMOOTHAF_ADAPTIVE_MAX_STEP : 1) &&
           result->reversals >= 0 && result->reversals <= max_reversals &&
           result->measures >= 1 && result->errors >= 0;
}

static void print_result(const char *controller, const struct bench_result *result)
{
    printf("  %-8s result=%-14s commands=%2d travel=%2d reversals=%2d "
           "measurements=%2d errors=%d elapsed_ticks=%u (SIMULATION)\n",
           controller, result_name(result->result), result->commands,
           result->travel, result->reversals, result->measures, result->errors,
           result->ticks);
}

static void benchmark_scenario(const struct bench_scenario *scenario)
{
    struct bench_result legacy;
    struct bench_result adaptive;
    int i;
    int invariant_failures = 0;

    /* Repetition confirms deterministic output; the printed row is one run. */
    for (i = 0; i < BENCH_REPETITIONS; i++)
    {
        struct bench_result legacy_i;
        struct bench_result adaptive_i;
        run_legacy_once(scenario, &legacy_i);
        run_adaptive_once(scenario, &adaptive_i);
        if (i == 0)
        {
            legacy = legacy_i;
            adaptive = adaptive_i;
        }
        else if (memcmp(&legacy, &legacy_i, sizeof(legacy)) != 0 ||
                memcmp(&adaptive, &adaptive_i, sizeof(adaptive)) != 0)
        {
            invariant_failures++;
        }
    }

    if (!invariant_ok(&legacy, 0) || !invariant_ok(&adaptive, 1))
        invariant_failures++;

    printf("Scenario: %s (same deterministic input, repetitions=%d)\n",
           scenario->name, BENCH_REPETITIONS);
    print_result("Legacy", &legacy);
    print_result("Adaptive", &adaptive);
    printf("  invariants=%s\n\n", invariant_failures ? "FAIL" : "PASS");
    if (invariant_failures)
        printf("BENCHMARK INVARIANT FAILURE\n");
}

int main(void)
{
    static const int unstable_return[] = { 900, 700, 800 };
    static const struct bench_scenario scenarios[] = {
        { .name = "Peak at +3", .initial_score = 700, .score_at = peak_at_3 },
        { .name = "Two-sided maximum at start", .initial_score = 1000, .score_at = peak_at_0 },
        { .name = "Wrong initial direction, peak at -3", .initial_score = 700, .score_at = peak_at_minus3 },
        { .name = "Flat scene", .initial_score = 1000, .score_at = flat },
        { .name = "Noisy deadband", .initial_score = 1000, .score_at = noisy },
        { .name = "Monotonic slope", .initial_score = 1000, .score_at = increasing },
        { .name = "Cancellation after two moves", .initial_score = 1000, .score_at = increasing, .cancel_after_moves = 2 },
        { .name = "Timeout after three moves", .initial_score = 1000, .score_at = increasing, .timeout_after_moves = 3 },
        { .name = "Lens failure on second move", .initial_score = 700, .score_at = peak_at_3, .fail_move_at = 2 },
        { .name = "Measure failure on first measurement", .initial_score = 700, .score_at = peak_at_3, .fail_measure_at = 1 },
        { .name = "Sharp peak", .initial_score = 500, .score_at = sharp_peak },
        { .name = "Unstable return sequence", .initial_score = 500, .score_at = peak_at_3, .sequence = unstable_return, .sequence_length = 3 },
    };
    size_t i;

    printf("SmoothAF comparative benchmark (SIMULATION)\n");
    printf("Ticks are synthetic controller/move/measurement workload units; "
           "no physical time is measured.\n");
    printf("travel is accumulated absolute displacement; measurements include "
           "the shared initial observation.\n\n");

    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        benchmark_scenario(&scenarios[i]);

    return 0;
}
