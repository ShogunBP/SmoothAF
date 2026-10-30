/*
 * SmoothAF Benchmark - Comparative Benchmarks with Circular Logging
 *
 * Host-side benchmark harness for the SmoothAF hill-climbing controller.
 * Uses fixed-size circular logging from smoothaf_diagnostics.h.
 * All timing is SIMULATED (tick counters), no physical time measurements.
 */

#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_core.h"
#include "../src/smoothaf_diagnostics.h"

/* Benchmark configuration */
#define SMOOTHAF_BENCH_ITERATIONS   1000
#define SMOOTHAF_BENCH_WARMUP       100

/* Benchmark result summary */
struct smoothaf_bench_result
{
    const char *name;
    int iterations;
    uint32_t total_ticks;
    uint32_t min_ticks;
    uint32_t max_ticks;
    uint32_t avg_ticks;
    int success_count;
    int error_count;
    int lock_count;
    int limit_count;
    int no_confidence_count;
    int cancel_count;
    int timeout_count;
    int lens_error_count;
    int measure_error_count;
};

typedef int (*smoothaf_bench_fn_t)(void *ctx);

/* Benchmark support: measure a function with simulated timing */
static inline void smoothaf_bench_run(const char *name,
                                      smoothaf_bench_fn_t fn,
                                      void *ctx,
                                      struct smoothaf_log_buffer *log,
                                      struct smoothaf_bench_result *result)
{
    result->name = name;
    result->iterations = 0;
    result->total_ticks = 0;
    result->min_ticks = 0xFFFFFFFF;
    result->max_ticks = 0;
    result->avg_ticks = 0;
    result->success_count = 0;
    result->error_count = 0;
    result->lock_count = 0;
    result->limit_count = 0;
    result->no_confidence_count = 0;
    result->cancel_count = 0;
    result->timeout_count = 0;
    result->lens_error_count = 0;
    result->measure_error_count = 0;

    if (log) smoothaf_log_set_enabled(log, 1);

    /* Warmup */
    for (int i = 0; i < SMOOTHAF_BENCH_WARMUP; i++)
    {
        fn(ctx);
    }

    /* Measured iterations */
    for (int i = 0; i < SMOOTHAF_BENCH_ITERATIONS; i++)
    {
        uint32_t start = log ? smoothaf_log_now(log) : 0;
        if (log) SMOOTHAF_LOG_WRITE_BENCH_START(log, i);

        int r = fn(ctx);

        uint32_t end = log ? smoothaf_log_now(log) : 0;
        uint32_t elapsed = end - start;
        if (log) SMOOTHAF_LOG_WRITE_BENCH_END(log, i, r);

        result->iterations++;
        result->total_ticks += elapsed;
        if (elapsed < result->min_ticks) result->min_ticks = elapsed;
        if (elapsed > result->max_ticks) result->max_ticks = elapsed;

        switch (r)
        {
            case 0: result->success_count++; break;   /* SMOOTHAF_RESULT_LOCKED */
            case 1: result->limit_count++; break;     /* SMOOTHAF_RESULT_LIMIT */
            case 2: result->no_confidence_count++; break; /* SMOOTHAF_RESULT_NO_CONFIDENCE */
            case 3: result->cancel_count++; break;    /* SMOOTHAF_RESULT_CANCELLED */
            case 4: result->timeout_count++; break;   /* SMOOTHAF_RESULT_TIMEOUT */
            case 5: result->lens_error_count++; break; /* SMOOTHAF_RESULT_LENS_ERROR */
            case 6: result->measure_error_count++; break; /* SMOOTHAF_RESULT_MEASURE_ERROR */
            default: result->error_count++; break;
        }
    }

    result->avg_ticks = result->iterations ? result->total_ticks / result->iterations : 0;

    if (log) smoothaf_log_set_enabled(log, 0);
}

/* Print benchmark summary to stdout (host test output) */
static inline void smoothaf_bench_print(const struct smoothaf_bench_result *r)
{
    printf("=== BENCHMARK: %s (SIMULATION) ===\n", r->name);
    printf("  Iterations:      %d\n", r->iterations);
    printf("  Total ticks:     %u\n", r->total_ticks);
    printf("  Min ticks:       %u\n", r->min_ticks);
    printf("  Max ticks:       %u\n", r->max_ticks);
    printf("  Avg ticks:       %u\n", r->avg_ticks);
    printf("  Results:\n");
    printf("    LOCKED:           %d\n", r->lock_count);
    printf("    LIMIT:            %d\n", r->limit_count);
    printf("    NO_CONFIDENCE:    %d\n", r->no_confidence_count);
    printf("    CANCELLED:        %d\n", r->cancel_count);
    printf("    TIMEOUT:          %d\n", r->timeout_count);
    printf("    LENS_ERROR:       %d\n", r->lens_error_count);
    printf("    MEASURE_ERROR:    %d\n", r->measure_error_count);
    printf("    OTHER_ERROR:      %d\n", r->error_count);
    printf("==========================================\n");
}

/* Print log entries (for debugging) */
static inline void smoothaf_log_print(const struct smoothaf_log_buffer *log)
{
    struct smoothaf_log_entry entries[SMOOTHAF_DIAG_CAPACITY];
    size_t n = smoothaf_log_read_all(log, entries, SMOOTHAF_DIAG_CAPACITY);

    printf("=== LOG BUFFER (SIMULATION, %zu entries) ===\n", n);
    for (size_t i = 0; i < n; i++)
    {
        const struct smoothaf_log_entry *e = &entries[i];
        const char *type_str = "UNKNOWN";
        switch (e->type)
        {
            case SMOOTHAF_LOG_MOVE: type_str = "MOVE"; break;
            case SMOOTHAF_LOG_MEASURE: type_str = "MEASURE"; break;
            case SMOOTHAF_LOG_CONTROLLER: type_str = "CONTROLLER"; break;
            case SMOOTHAF_LOG_ABORT: type_str = "ABORT"; break;
            case SMOOTHAF_LOG_ERROR: type_str = "ERROR"; break;
            case SMOOTHAF_LOG_BENCH_START: type_str = "BENCH_START"; break;
            case SMOOTHAF_LOG_BENCH_END: type_str = "BENCH_END"; break;
        }
        printf("  [%u] %s: v1=%d v2=%d v3=%d\n",
               e->timestamp_ms, type_str, e->value1, e->value2, e->value3);
    }
    if (log->dropped)
    {
        printf("  ... %zu entries dropped (wrap-around)\n", log->dropped);
    }
    printf("==============================================\n");
}

/* ============================================================
 * Simulation Contexts for Different Scenarios
 * ============================================================ */

struct bench_ctx
{
    struct smoothaf_hc hc;
    struct
    {
        int pos;
        int moves;
        int measures;
        int seq_index;
    } sim;
    struct smoothaf_log_buffer log;
    int initial_score;
    int (*score_fn)(int pos);
    const int *seq;
    int seq_len;
    int cancel_at_move;
    int timeout_at_move;
    int fail_move_at;
    int fail_measure_at;
    int cancel_during_measure;
    int timeout_during_measure;
};

/* Score functions for different scene types */
static int score_peak_at_3(int pos)    { return 1000 - 100 * (pos > 3 ? pos - 3 : 3 - pos); }
static int score_peak_at_0(int pos)    { return 1000 - 100 * (pos > 0 ? pos : -pos); }
static int score_peak_at_minus3(int pos) { return 1000 - 100 * (pos > -3 ? pos + 3 : -3 - pos); }
static int score_increasing(int pos)   { return 1000 + 100 * pos; }
static int score_flat(int pos)         { (void)pos; return 1000; }
static int score_noisy(int pos)        { if (pos > 0) return 1010; if (pos < 0) return 990; return 1000; }
static int score_one_flat_side(int pos){ if (pos > 0) return 1000; if (pos < 0) return 900; return 1000; }
static int score_sharp_peak(int pos)   { return 1000 - 500 * (pos > 2 ? pos - 2 : 2 - pos); }
static int score_broad_peak(int pos)   { return 1000 - 20 * (pos > 5 ? pos - 5 : 5 - pos); }
static int score_double_peak(int pos)  {
    int d1 = pos > 3 ? pos - 3 : 3 - pos;
    int d2 = pos > -3 ? pos + 3 : -3 - pos;
    int v1 = 1000 - 100 * d1;
    int v2 = 900 - 100 * d2;
    return v1 > v2 ? v1 : v2;
}
static int score_stepped(int pos)      { return 1000 + (pos / 2) * 50; }

/* Simulator move function with logging */
static int bench_move(void *ctx, int dir)
{
    struct bench_ctx *b = (struct bench_ctx *)ctx;
    b->sim.moves++;

    if (b->fail_move_at && b->sim.moves == b->fail_move_at)
    {
        SMOOTHAF_LOG_WRITE_ERROR(&b->log, 5); /* LENS_ERROR */
        return 0;
    }

    b->sim.pos += dir;
    SMOOTHAF_LOG_WRITE_MOVE(&b->log, dir, b->sim.pos);
    return 1;
}

/* Simulator measure function with logging */
static int bench_measure(void *ctx, int *score)
{
    struct bench_ctx *b = (struct bench_ctx *)ctx;
    b->sim.measures++;

    if (b->cancel_during_measure && b->sim.measures == b->cancel_during_measure)
    {
        b->cancel_at_move = b->sim.moves;
    }
    if (b->timeout_during_measure && b->sim.measures == b->timeout_during_measure)
    {
        b->timeout_at_move = b->sim.moves;
    }
    if (b->fail_measure_at && b->sim.measures == b->fail_measure_at)
    {
        SMOOTHAF_LOG_WRITE_ERROR(&b->log, 6); /* MEASURE_ERROR */
        return 0;
    }

    if (b->seq && b->sim.seq_index < b->seq_len)
    {
        *score = b->seq[b->sim.seq_index++];
    }
    else if (b->score_fn)
    {
        *score = b->score_fn(b->sim.pos);
    }
    else
    {
        *score = 0;
    }

    SMOOTHAF_LOG_WRITE_MEASURE(&b->log, *score, 128, b->sim.pos);
    return 1;
}

/* Simulator abort function with logging */
static int bench_abort(void *ctx)
{
    struct bench_ctx *b = (struct bench_ctx *)ctx;

    if (b->cancel_at_move && b->sim.moves >= b->cancel_at_move)
    {
        SMOOTHAF_LOG_WRITE_ABORT(&b->log, 3); /* CANCELLED */
        return 3; /* SMOOTHAF_RESULT_CANCELLED */
    }
    if (b->timeout_at_move && b->sim.moves >= b->timeout_at_move)
    {
        SMOOTHAF_LOG_WRITE_ABORT(&b->log, 4); /* TIMEOUT */
        return 4; /* SMOOTHAF_RESULT_TIMEOUT */
    }
    return 0;
}

/* Benchmark function wrapper */
static int bench_run_iteration(void *ctx)
{
    struct bench_ctx *b = (struct bench_ctx *)ctx;

    smoothaf_log_tick(&b->log, 1); /* Simulated tick per iteration start */

    smoothaf_hc_init(&b->hc, b->initial_score, 1);
    enum smoothaf_hc_action action = smoothaf_hc_next_move(&b->hc);

    /* Reset simulator state per iteration */
    b->sim.pos = 0;
    b->sim.moves = 0;
    b->sim.measures = 0;
    b->sim.seq_index = 0;
    b->cancel_at_move = 0;
    b->timeout_at_move = 0;

    for (;;)
    {
        smoothaf_log_tick(&b->log, 1);

        int stop = bench_abort(b);
        if (stop)
        {
            return stop;
        }

        if (action == SMOOTHAF_HC_MOVE)
        {
            smoothaf_log_tick(&b->log, 5); /* Simulated move time */
            if (!bench_move(b, b->hc.dir))
            {
                return 5; /* LENS_ERROR */
            }

            stop = bench_abort(b);
            if (stop) return stop;
        }

        smoothaf_log_tick(&b->log, 2); /* Simulated settle time */
        int score = 0;
        if (!bench_measure(b, &score))
        {
            return 6; /* MEASURE_ERROR */
        }

        smoothaf_log_tick(&b->log, 1);
        stop = bench_abort(b);
        if (stop) return stop;

        SMOOTHAF_LOG_WRITE_CONTROLLER(&b->log, action, b->hc.best, b->hc.best_pos, b->hc.pos, b->hc.dir);

        action = smoothaf_hc_on_score(&b->hc, score);

        if (action == SMOOTHAF_HC_LOCKED) return 0;      /* LOCKED */
        if (action == SMOOTHAF_HC_LIMIT) return 1;       /* LIMIT */
        if (action == SMOOTHAF_HC_NO_CONFIDENCE) return 2; /* NO_CONFIDENCE */
    }
}

/* ============================================================
 * Benchmark Scenarios
 * ============================================================ */

static void run_benchmark(const char *name,
                          int initial_score,
                          int (*score_fn)(int pos),
                          int cancel_at,
                          int timeout_at,
                          int fail_move_at,
                          int fail_measure_at,
                          int cancel_during_measure,
                          int timeout_during_measure,
                          const int *seq, int seq_len)
{
    struct bench_ctx b;
    struct smoothaf_bench_result result;

    memset(&b, 0, sizeof(b));
    b.initial_score = initial_score;
    b.score_fn = score_fn;
    b.seq = seq;
    b.seq_len = seq_len;
    b.cancel_at_move = cancel_at;
    b.timeout_at_move = timeout_at;
    b.fail_move_at = fail_move_at;
    b.fail_measure_at = fail_measure_at;
    b.cancel_during_measure = cancel_during_measure;
    b.timeout_during_measure = timeout_during_measure;

    smoothaf_log_init(&b.log);
    smoothaf_log_set_enabled(&b.log, 1);

    smoothaf_bench_run(name, bench_run_iteration, &b, &b.log, &result);
    smoothaf_bench_print(&result);

    /* Print last few log entries for verification */
    smoothaf_log_print(&b.log);
}

int main(void)
{
    printf("\n");
    printf("============================================================\n");
    printf("  SmoothAF Benchmark Suite (SIMULATION)\n");
    printf("  All timing values are simulated tick counters.\n");
    printf("  No physical time measurements are performed.\n");
    printf("============================================================\n\n");

    /* Scenario 1: Normal peak at +3 */
    const char *name1 = "Normal Peak at +3";
    run_benchmark(name1, 700, score_peak_at_3, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 2: Peak at start (two-sided check) */
    const char *name2 = "Two-Sided Maximum at Start";
    run_benchmark(name2, 1000, score_peak_at_0, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 3: Wrong initial direction */
    const char *name3 = "Wrong Initial Direction (Peak at -3)";
    run_benchmark(name3, 700, score_peak_at_minus3, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 4: Flat scene - no texture */
    const char *name4 = "Flat Scene (No Texture)";
    run_benchmark(name4, 1000, score_flat, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 5: Noisy measurements within deadband */
    const char *name5 = "Noisy Measurements (Within Deadband)";
    run_benchmark(name5, 1000, score_noisy, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 6: One side flat, other worse - inconclusive */
    const char *name6 = "One Flat Side (Inconclusive)";
    run_benchmark(name6, 1000, score_one_flat_side, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 7: Monotonic improvement - hits command limit */
    const char *name7 = "Monotonic Slope (Command Limit)";
    run_benchmark(name7, 1000, score_increasing, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 8: Cancellation after 2 moves */
    const char *name8 = "Cancellation After 2 Moves";
    run_benchmark(name8, 1000, score_increasing, 2, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 9: Timeout after 3 moves */
    const char *name9 = "Timeout After 3 Moves";
    run_benchmark(name9, 1000, score_increasing, 0, 3, 0, 0, 0, 0, NULL, 0);

    /* Scenario 10: Cancel during locking measurement */
    const char *name10 = "Cancel During Locking Measurement";
    run_benchmark(name10, 1000, score_peak_at_0, 0, 0, 0, 0, 2, 0, NULL, 0);

    /* Scenario 11: Timeout during locking measurement */
    const char *name11 = "Timeout During Locking Measurement";
    run_benchmark(name11, 1000, score_peak_at_0, 0, 0, 0, 0, 0, 2, NULL, 0);

    /* Scenario 12: Lens failure on 2nd move */
    const char *name12 = "Lens Failure on 2nd Move";
    run_benchmark(name12, 700, score_peak_at_3, 0, 0, 2, 0, 0, 0, NULL, 0);

    /* Scenario 13: Measure failure on 1st measure */
    const char *name13 = "Measure Failure on 1st Measure";
    run_benchmark(name13, 700, score_peak_at_3, 0, 0, 0, 1, 0, 0, NULL, 0);

    /* Scenario 14: Unstable return to best position */
    const char *name14 = "Unstable Return to Best Position";
    {
        int unstable_seq[] = { 900, 700, 800 };
        run_benchmark(name14, 500, NULL, 0, 0, 0, 0, 0, 0, unstable_seq, 3);
    }

    /* Scenario 15: Unstable two-sided check */
    const char *name15 = "Unstable Two-Sided Check";
    {
        int low_seq[] = { 900, 800, 900, 800 };
        run_benchmark(name15, 1000, NULL, 0, 0, 0, 0, 0, 0, low_seq, 4);
    }

    /* Scenario 16: Sharp narrow peak */
    const char *name16 = "Sharp Narrow Peak";
    run_benchmark(name16, 500, score_sharp_peak, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 17: Broad shallow peak */
    const char *name17 = "Broad Shallow Peak";
    run_benchmark(name17, 900, score_broad_peak, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 18: Double peak (global at +3, local at -3) */
    const char *name18 = "Double Peak (Global at +3)";
    run_benchmark(name18, 700, score_double_peak, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 19: Stepped improvement */
    const char *name19 = "Stepped Improvement";
    run_benchmark(name19, 1000, score_stepped, 0, 0, 0, 0, 0, 0, NULL, 0);

    /* Scenario 20: Direction zero normalization */
    const char *name20 = "Direction Zero Normalization";
    run_benchmark(name20, 700, score_peak_at_3, 0, 0, 0, 0, 0, 0, NULL, 0);

    printf("\n");
    printf("============================================================\n");
    printf("  All benchmarks completed (SIMULATION)\n");
    printf("============================================================\n");

    return 0;
}