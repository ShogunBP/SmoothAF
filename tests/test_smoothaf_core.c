/*
 * Host-side tests for the hardware-independent SmoothAF core.
 *
 * These tests do NOT build or run the Magic Lantern module.  They validate:
 *   - UYVY luminance indexing;
 *   - ROI alignment and geometry limits;
 *   - SAD and Laplacian metrics against independent reference loops;
 *   - mean luminance and confidence gates;
 *   - the bounded hill-climbing controller (peak, wrong direction, flat scene,
 *     noise, cancellation, timeout, lens failure, measure failure, limits and
 *     an inconclusive return to the best position).
 *
 * Build (MSVC):
 *   cl /W4 /Fe:test_smoothaf_core.exe test_smoothaf_core.c
 *
 * Build (GCC/Clang):
 *   gcc -std=c99 -Wall -Wextra -O2 -o test_smoothaf_core test_smoothaf_core.c
 */

#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_core.h"
#include "../src/smoothaf_diagnostics.h"

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

struct auto_mock
{
    int position;
    int commands;
    int directions[SMOOTHAF_AUTO_MAX_COMMANDS];
    int scores[SMOOTHAF_AUTO_MAX_COMMANDS];
    int statuses[SMOOTHAF_AUTO_MAX_COMMANDS];
    int samples;
    int stop;
};

static int auto_move(void *ctx, int direction)
{
    struct auto_mock *m = (struct auto_mock *)ctx;
    if (m->commands >= SMOOTHAF_AUTO_MAX_COMMANDS) return 0;
    m->directions[m->commands++] = direction;
    m->position += direction;
    return 1;
}

static int auto_measure(void *ctx, int *score)
{
    struct auto_mock *m = (struct auto_mock *)ctx;
    int index = m->samples++;
    if (index >= SMOOTHAF_AUTO_MAX_COMMANDS) return SMOOTHAF_AUTO_MEASURE_INVALID;
    *score = m->scores[index];
    return m->statuses[index];
}

static int auto_abort(void *ctx)
{
    return ((struct auto_mock *)ctx)->stop;
}

static void test_auto_probe(void)
{
    struct smoothaf_auto_probe_config config;
    struct smoothaf_auto_probe probe;
    struct auto_mock mock;
    memset(&config, 0, sizeof(config));
    config.first_direction = -1;
    config.initial_score = 100;
    config.max_commands = SMOOTHAF_AUTO_MAX_COMMANDS;
    config.max_reversals = SMOOTHAF_AUTO_MAX_REVERSALS;

    memset(&mock, 0, sizeof(mock));
    mock.scores[0] = 140;
    mock.statuses[0] = SMOOTHAF_AUTO_MEASURE_VALID;
    CHECK(smoothaf_auto_probe_run(&probe, &config, auto_move, auto_measure,
                                  auto_abort, &mock) == SMOOTHAF_AUTO_FIRST_IMPROVED,
          "AUTO accepts first-side improvement");
    CHECK(probe.selected_direction == -1 && probe.commands == 2 &&
          mock.position == 0 && mock.directions[0] == -1 &&
          mock.directions[1] == 1, "AUTO restores origin after first probe");

    memset(&mock, 0, sizeof(mock));
    mock.scores[0] = 60;
    mock.scores[1] = 150;
    mock.statuses[0] = SMOOTHAF_AUTO_MEASURE_VALID;
    mock.statuses[1] = SMOOTHAF_AUTO_MEASURE_VALID;
    CHECK(smoothaf_auto_probe_run(&probe, &config, auto_move, auto_measure,
                                  auto_abort, &mock) == SMOOTHAF_AUTO_OPPOSITE_IMPROVED,
          "AUTO selects opposite-side improvement");
    CHECK(probe.selected_direction == 1 && probe.commands == 4 &&
          mock.position == 0 && mock.directions[0] == -1 &&
          mock.directions[1] == 1 && mock.directions[2] == 1 &&
          mock.directions[3] == -1, "AUTO compensates both probes");

    memset(&mock, 0, sizeof(mock));
    mock.scores[0] = 100;
    mock.scores[1] = 101;
    mock.statuses[0] = SMOOTHAF_AUTO_MEASURE_VALID;
    mock.statuses[1] = SMOOTHAF_AUTO_MEASURE_VALID;
    CHECK(smoothaf_auto_probe_run(&probe, &config, auto_move, auto_measure,
                                  auto_abort, &mock) == SMOOTHAF_AUTO_UNCERTAIN,
          "AUTO reports uncertainty for flat valid responses");
    CHECK(probe.selected_direction == 0 && mock.position == 0,
          "uncertain AUTO leaves origin");

    memset(&mock, 0, sizeof(mock));
    mock.statuses[0] = SMOOTHAF_AUTO_MEASURE_LOW;
    CHECK(smoothaf_auto_probe_run(&probe, &config, auto_move, auto_measure,
                                  auto_abort, &mock) == SMOOTHAF_AUTO_LOW_CONFIDENCE,
          "AUTO distinguishes low confidence");
    CHECK(mock.commands == 2 && mock.position == 0,
          "low confidence does not probe opposite side");
}

static int abs_i(int v)
{
    return v < 0 ? -v : v;
}

static void test_uyvy_offsets(void)
{
    uint8_t row[16];
    int x;

    memset(row, 0xEE, sizeof(row));
    row[0] = 0xAA; /* U0 */
    row[1] = 10;   /* Y0 */
    row[2] = 0xBB; /* V0 */
    row[3] = 20;   /* Y1 */
    row[4] = 0xAA; /* U2 */
    row[5] = 30;   /* Y2 */
    row[6] = 0xBB; /* V2 */
    row[7] = 40;   /* Y3 */

    CHECK(smoothaf_y_offset(0) == 1, "Y offset for x=0");
    CHECK(smoothaf_y_offset(1) == 3, "Y offset for x=1");
    CHECK(smoothaf_y_offset(2) == 5, "Y offset for x=2");
    CHECK(smoothaf_y_offset(3) == 7, "Y offset for x=3");

    for (x = 0; x < 4; x++)
    {
        CHECK(smoothaf_y(row, x) == 10 + 10 * x, "Y value is not U/V");
    }

    /* U and V deliberately differ from Y, so odd pixels catch bad indexing. */
    CHECK(smoothaf_y(row, 0) != 0xAA && smoothaf_y(row, 0) != 0xBB,
          "x=0 does not read U/V");
    CHECK(smoothaf_y(row, 1) != 0xAA && smoothaf_y(row, 1) != 0xBB,
          "x=1 does not read U/V");
    CHECK(smoothaf_y(row, 2) != 0xAA && smoothaf_y(row, 2) != 0xBB,
          "x=2 does not read U/V");
    CHECK(smoothaf_y(row, 3) != 0xAA && smoothaf_y(row, 3) != 0xBB,
          "x=3 does not read U/V");
}

static void test_roi_and_geometry(void)
{
    int width;
    int height;

    for (width = SMOOTHAF_ROI_SIZE; width <= 1000; width++)
    {
        int left = smoothaf_roi_left(width);
        CHECK((left & 1) == 0, "ROI left must be even");
        CHECK(left >= 0, "ROI left must be non-negative");
        CHECK(left + SMOOTHAF_ROI_SIZE <= width, "ROI fits horizontally");
    }

    for (height = SMOOTHAF_ROI_SIZE; height <= 1000; height++)
    {
        int top = smoothaf_roi_top(height);
        CHECK(top >= 0, "ROI top must be non-negative");
        CHECK(top + SMOOTHAF_ROI_SIZE <= height, "ROI fits vertically");
    }

    CHECK(smoothaf_geometry_ok(720, 480, 1440), "720x480 pitch 1440 is valid");
    CHECK(smoothaf_geometry_ok(64, 64, 128), "64x64 pitch 128 is valid");
    CHECK(!smoothaf_geometry_ok(63, 64, 126), "width smaller than ROI");
    CHECK(!smoothaf_geometry_ok(64, 63, 128), "height smaller than ROI");
    CHECK(!smoothaf_geometry_ok(64, 64, 127), "pitch smaller than width*2");
    CHECK(!smoothaf_geometry_ok(64, 64, 129), "odd pitch is rejected");
    CHECK(!smoothaf_geometry_ok(720, 480, 1441), "odd pitch at 720 is rejected");
    CHECK(!smoothaf_geometry_ok(9000, 480, 18000), "absurd width is rejected");
}

static void fill_uyvy(uint8_t *buf, int width, int height, int pitch)
{
    int x;
    int y;

    for (y = 0; y < height; y++)
    {
        uint8_t *row = buf + y * pitch;
        for (x = 0; x < width; x += 2)
        {
            row[x * 2] = 0x11;                                   /* U */
            row[x * 2 + 1] = (uint8_t)((x * 7 + y * 3) & 0xFF);  /* Y0 */
            row[x * 2 + 2] = 0x22;                               /* V */
            row[x * 2 + 3] = (uint8_t)(((x + 1) * 7 + y * 3) & 0xFF); /* Y1 */
        }
    }
}

static int ref_sad(const uint8_t *base, int pitch)
{
    int score = 0;
    int x;
    int y;

    for (y = 0; y < SMOOTHAF_ROI_SIZE - 2; y += SMOOTHAF_SAMPLE_STEP)
    {
        const uint8_t *row = base + y * pitch;
        const uint8_t *row2 = row + 2 * pitch;
        for (x = 0; x < SMOOTHAF_ROI_SIZE - 2; x += SMOOTHAF_SAMPLE_STEP)
        {
            int c = row[x * 2 + 1];
            score += abs_i(row[(x + 2) * 2 + 1] - c);
            score += abs_i(row2[x * 2 + 1] - c);
        }
    }
    return score;
}

static int ref_mean(const uint8_t *base, int pitch)
{
    int sum = 0;
    int count = 0;
    int x;
    int y;

    for (y = 0; y < SMOOTHAF_ROI_SIZE - 2; y += SMOOTHAF_SAMPLE_STEP)
    {
        const uint8_t *row = base + y * pitch;
        for (x = 0; x < SMOOTHAF_ROI_SIZE - 2; x += SMOOTHAF_SAMPLE_STEP)
        {
            sum += row[x * 2 + 1];
            count++;
        }
    }
    return count ? sum / count : 0;
}

static int ref_laplacian(const uint8_t *base, int pitch)
{
    int score = 0;
    int x;
    int y;

    for (y = 1; y < SMOOTHAF_ROI_SIZE - 1; y += SMOOTHAF_SAMPLE_STEP)
    {
        const uint8_t *row = base + y * pitch;
        for (x = 1; x < SMOOTHAF_ROI_SIZE - 1; x += SMOOTHAF_SAMPLE_STEP)
        {
            int c = row[x * 2 + 1];
            int up = row[-pitch + x * 2 + 1];
            int down = row[pitch + x * 2 + 1];
            int left = row[(x - 1) * 2 + 1];
            int right = row[(x + 1) * 2 + 1];
            score += abs_i(up + down + left + right - 4 * c);
        }
    }
    return score;
}

static void test_metrics(void)
{
    uint8_t buf[64 * 128];
    int mean = -1;
    int score;
    int x;
    int y;

    /* Flat image: both metrics and the mean must behave predictably. */
    for (y = 0; y < 64; y++)
    {
        uint8_t *row = buf + y * 128;
        for (x = 0; x < 64; x += 2)
        {
            row[x * 2] = 0xFF;   /* U: must never be read as Y */
            row[x * 2 + 1] = 128;
            row[x * 2 + 2] = 0xFF; /* V */
            row[x * 2 + 3] = 128;
        }
    }

    score = smoothaf_measure_roi(buf, 128, SMOOTHAF_METRIC_SAD, &mean);
    CHECK(score == 0, "flat SAD is zero");
    CHECK(mean == 128, "flat mean is 128");

    score = smoothaf_measure_roi(buf, 128, SMOOTHAF_METRIC_LAPLACIAN, &mean);
    CHECK(score == 0, "flat Laplacian is zero");
    CHECK(mean == 128, "flat Laplacian mean is 128");

    /* Black Y with bright U/V: proves U/V are not used by the metric. */
    memset(buf, 0, sizeof(buf));
    for (y = 0; y < 64; y++)
    {
        uint8_t *row = buf + y * 128;
        for (x = 0; x < 64; x += 2)
        {
            row[x * 2] = 255;
            row[x * 2 + 2] = 255;
        }
    }
    score = smoothaf_measure_roi(buf, 128, SMOOTHAF_METRIC_SAD, &mean);
    CHECK(score == 0, "SAD ignores U/V");
    CHECK(mean == 0, "mean ignores U/V");

    /* Compare both metrics against independent reference loops. */
    fill_uyvy(buf, 64, 64, 128);
    score = smoothaf_measure_roi(buf, 128, SMOOTHAF_METRIC_SAD, &mean);
    CHECK(score == ref_sad(buf, 128), "SAD matches reference");
    CHECK(mean == ref_mean(buf, 128), "mean matches reference");
    score = smoothaf_measure_roi(buf, 128, SMOOTHAF_METRIC_LAPLACIAN, &mean);
    CHECK(score == ref_laplacian(buf, 128), "Laplacian matches reference");

    /* Unknown metric falls back to SAD. */
    score = smoothaf_measure_roi(buf, 128, 12345, &mean);
    CHECK(score == ref_sad(buf, 128), "unknown metric falls back to SAD");

    CHECK(!smoothaf_has_confidence(1000, 8), "dark scene has no confidence");
    CHECK(!smoothaf_has_confidence(10, 128), "flat scene has no confidence");
    CHECK(smoothaf_has_confidence(1000, 128), "normal scene has confidence");
}

/* ------------------------------------------------------------------ */
/* Hill-climbing simulator                                            */
/* ------------------------------------------------------------------ */

struct sim_ctx
{
    int pos;
    int moves;
    int measures;
    int fail_move_at;
    int fail_measure_at;
    int cancel_at;
    int timeout_at;
    int cancel_during_measure;
    int timeout_during_measure;
    const int *seq;
    int seq_len;
    int seq_index;
    int (*score_at)(int pos);
};

static int sim_move(void *ctx, int dir)
{
    struct sim_ctx *s = (struct sim_ctx *)ctx;
    s->moves++;
    if (s->fail_move_at && s->moves == s->fail_move_at)
    {
        return 0;
    }
    s->pos += dir;
    return 1;
}

static int sim_measure(void *ctx, int *score)
{
    struct sim_ctx *s = (struct sim_ctx *)ctx;
    s->measures++;
    if (s->cancel_during_measure &&
        s->measures == s->cancel_during_measure)
    {
        s->cancel_at = s->moves;
    }
    if (s->timeout_during_measure &&
        s->measures == s->timeout_during_measure)
    {
        s->timeout_at = s->moves;
    }
    if (s->fail_measure_at && s->measures == s->fail_measure_at)
    {
        return 0;
    }
    if (s->seq && s->seq_index < s->seq_len)
    {
        *score = s->seq[s->seq_index++];
    }
    else
    {
        *score = s->score_at ? s->score_at(s->pos) : 0;
    }
    return 1;
}

static int sim_abort(void *ctx)
{
    struct sim_ctx *s = (struct sim_ctx *)ctx;
    if (s->cancel_at && s->moves >= s->cancel_at)
    {
        return SMOOTHAF_RESULT_CANCELLED;
    }
    if (s->timeout_at && s->moves >= s->timeout_at)
    {
        return SMOOTHAF_RESULT_TIMEOUT;
    }
    return 0;
}

static int peak3(int pos)
{
    return 1000 - 100 * abs_i(pos - 3);
}

static int peak0(int pos)
{
    return 1000 - 100 * abs_i(pos);
}

static int peak_minus3(int pos)
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

static int one_flat_side(int pos)
{
    if (pos > 0) return 1000;
    if (pos < 0) return 900;
    return 1000;
}

static void sim_reset(struct sim_ctx *s)
{
    memset(s, 0, sizeof(*s));
}

static void test_safe_mode_panic(void)
{
    volatile int enabled = 1;
    volatile int cancel = 0;
    volatile int panic = 0;

    CHECK(smoothaf_can_start(enabled, panic, 0),
          "safe mode allows enabled idle start");
    smoothaf_panic_latch(&enabled, &cancel, &panic);
    CHECK(panic && cancel && !enabled,
          "panic disables and cancels immediately");
    CHECK(!smoothaf_can_start(1, panic, 0),
          "panic blocks a new task");
    smoothaf_panic_rearm(&cancel, &panic);
    CHECK(!panic && !cancel, "rearm clears panic and cancellation");
    CHECK(!smoothaf_can_start(enabled, panic, 0),
          "rearm leaves safe mode disabled");
}

static void test_enable_and_task_gate(void)
{
    CHECK(!smoothaf_can_start(0, 0, 0), "movement starts disabled");
    CHECK(smoothaf_can_start(1, 0, 0), "enabled movement may reserve a task");
    CHECK(!smoothaf_can_start(1, 0, 1), "active task blocks a second task");
    CHECK(SMOOTHAF_MAX_COMMANDS == 16, "command limit is preserved");
    CHECK(SMOOTHAF_MAX_REVERSALS == 4, "reversal limit is preserved");
}

static void test_measure_route(void)
{
    CHECK(smoothaf_measure_route_for(0, 0, 0) ==
          SMOOTHAF_MEASURE_ROUTE_DIRECT,
          "idle Measure starts its own task");
    CHECK(smoothaf_measure_route_for(0, 0, 1) ==
          SMOOTHAF_MEASURE_ROUTE_DIRECT,
          "idle Measure does not queue even if the monitor advertises readiness");
    CHECK(smoothaf_measure_route_for(0, 1, 1) ==
          SMOOTHAF_MEASURE_ROUTE_QUEUED,
          "Measure queues on the running monitor task");
    CHECK(smoothaf_measure_route_for(0, 1, 0) ==
          SMOOTHAF_MEASURE_ROUTE_BLOCKED,
          "Measure does not race another focus task");
    CHECK(smoothaf_measure_route_for(1, 0, 0) ==
          SMOOTHAF_MEASURE_ROUTE_BLOCKED,
          "panic blocks Measure start");
    CHECK(smoothaf_measure_route_for(1, 1, 1) ==
          SMOOTHAF_MEASURE_ROUTE_BLOCKED,
          "panic blocks the queued monitor route");
}

static void test_measure_all_metrics(void)
{
    uint8_t buf[64 * 128];
    int sad;
    int laplacian;
    int mean;

    fill_uyvy(buf, 64, 64, 128);
    smoothaf_measure_all(buf, 128, &sad, &laplacian, &mean);
    CHECK(sad == ref_sad(buf, 128), "combined SAD matches reference");
    CHECK(laplacian == ref_laplacian(buf, 128),
          "combined Laplacian matches reference");
    CHECK(mean == ref_mean(buf, 128), "combined mean matches reference");
}

static void test_monitor_states_are_passive(void)
{
    CHECK(sizeof(struct smoothaf_telemetry) <= 32,
          "telemetry remains a small fixed-RAM record");
    CHECK(SMOOTHAF_METRIC_SAD == 0, "monitor uses existing metric contract");
    CHECK(SMOOTHAF_METRIC_LAPLACIAN == 1,
          "monitor exposes the existing Laplacian contract");
}

static void test_hill_climb(void)
{
    struct smoothaf_hc hc;
    struct sim_ctx s;
    enum smoothaf_result r;
    const int unstable_return[] = { 900, 700, 800 };
    const int low_return[] = { 900, 800, 900, 800 };

    /* Normal peak at +3. */
    sim_reset(&s);
    s.score_at = peak3;
    r = smoothaf_hc_run(&hc, peak3(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LOCKED, "peak at +3 locks");
    CHECK(hc.best_pos == 3, "best position is +3");
    CHECK(hc.best == 1000, "best score is 1000");
    CHECK(hc.commands <= SMOOTHAF_MAX_COMMANDS, "commands within limit");
    CHECK(hc.reversals <= SMOOTHAF_MAX_REVERSALS, "reversals within limit");

    /* Initial position is already the peak: both sides are worse. */
    sim_reset(&s);
    s.score_at = peak0;
    r = smoothaf_hc_run(&hc, peak0(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LOCKED, "two-sided maximum at start locks");
    CHECK(hc.best_pos == 0, "best position remains 0");
    CHECK(hc.commands <= 4, "two-sided check uses few commands");

    /* Initial direction is wrong. */
    sim_reset(&s);
    s.score_at = peak_minus3;
    r = smoothaf_hc_run(&hc, peak_minus3(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LOCKED, "wrong initial direction recovers");
    CHECK(hc.best_pos == -3, "best position is -3");

    /* Flat scene: no texture, no false LOCKED. */
    sim_reset(&s);
    s.score_at = flat;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_NO_CONFIDENCE, "flat scene has no confidence");
    CHECK(r != SMOOTHAF_RESULT_LOCKED, "flat scene never locks");

    /* Small noise inside the deadband must not look like an improvement. */
    sim_reset(&s);
    s.score_at = noisy;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_NO_CONFIDENCE, "noise is rejected");

    /* One side flat, the other worse: inconclusive. */
    sim_reset(&s);
    s.score_at = one_flat_side;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_NO_CONFIDENCE, "flat side is inconclusive");

    /* Monotonic improvement: bounded by the command limit. */
    sim_reset(&s);
    s.score_at = increasing;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LIMIT, "monotonic slope hits the limit");
    CHECK(hc.commands == SMOOTHAF_MAX_COMMANDS, "exactly 16 commands");

    /* Stop/cancel is observed before another move. */
    sim_reset(&s);
    s.score_at = increasing;
    s.cancel_at = 2;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_CANCELLED, "cancel is reported");
    CHECK(hc.commands == 2, "no move after cancel");

    /* Timeout is observed before another move. */
    sim_reset(&s);
    s.score_at = increasing;
    s.timeout_at = 3;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_TIMEOUT, "timeout is reported");
    CHECK(hc.commands == 3, "no move after timeout");

    /* Cancellation during the measurement that would otherwise confirm the
     * local maximum must not return LOCKED. */
    sim_reset(&s);
    s.score_at = peak0;
    s.cancel_during_measure = 2;
    r = smoothaf_hc_run(&hc, peak0(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_CANCELLED,
          "cancel during locking measurement is reported");
    CHECK(r != SMOOTHAF_RESULT_LOCKED,
          "cancel during locking measurement never locks");
    CHECK(hc.commands == 2, "cancel during measurement adds no movement");

    /* Timeout during a measurement must also be observed before evaluation. */
    sim_reset(&s);
    s.score_at = peak0;
    s.timeout_during_measure = 2;
    r = smoothaf_hc_run(&hc, peak0(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_TIMEOUT,
          "timeout during locking measurement is reported");
    CHECK(r != SMOOTHAF_RESULT_LOCKED,
          "timeout during locking measurement never locks");
    CHECK(hc.commands == 2, "timeout during measurement adds no movement");

    /* Cancellation after a movement is observed before measuring again. */
    sim_reset(&s);
    s.score_at = increasing;
    s.cancel_at = 1;
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_CANCELLED,
          "cancel after movement is reported before measurement");
    CHECK(hc.commands == 1, "cancel after movement prevents another command");

    /* Simulated lens failure. */
    sim_reset(&s);
    s.score_at = peak3;
    s.fail_move_at = 2;
    r = smoothaf_hc_run(&hc, peak3(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LENS_ERROR, "lens failure is reported");
    CHECK(hc.commands == 2, "lens failure stops movement");

    /* Simulated measure failure. */
    sim_reset(&s);
    s.score_at = peak3;
    s.fail_measure_at = 1;
    r = smoothaf_hc_run(&hc, peak3(0), 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_MEASURE_ERROR, "measure failure is reported");

    /* Return to the best position cannot be confirmed. */
    sim_reset(&s);
    s.seq = unstable_return;
    s.seq_len = (int)(sizeof(unstable_return) / sizeof(unstable_return[0]));
    r = smoothaf_hc_run(&hc, 500, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LIMIT, "unconfirmed return is inconclusive");

    /* Two-sided maximum, but the return measurement is unstable. */
    sim_reset(&s);
    s.seq = low_return;
    s.seq_len = (int)(sizeof(low_return) / sizeof(low_return[0]));
    r = smoothaf_hc_run(&hc, 1000, 1, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LIMIT, "unstable two-sided check is inconclusive");

    /* Direction zero is normalized to a valid direction. */
    sim_reset(&s);
    s.score_at = peak3;
    r = smoothaf_hc_run(&hc, peak3(0), 0, sim_move, sim_measure, sim_abort, &s);
    CHECK(r == SMOOTHAF_RESULT_LOCKED, "direction zero is normalized");
    CHECK(hc.dir == -1 || hc.dir == 1, "direction is never zero");
}

static void test_telemetry(void)
{
    struct smoothaf_telemetry telemetry;

    smoothaf_telemetry_init(&telemetry);
    CHECK(telemetry.score == 0 && telemetry.duration_ms == 0,
          "telemetry initializes to zero");
    telemetry.score = 1234;
    telemetry.duration_ms = 42;
    telemetry.direction = -1;
    telemetry.step = 2;
    telemetry.state = 3;
    telemetry.error = 1;
    telemetry.reason = 7;
    CHECK(telemetry.score == 1234 && telemetry.duration_ms == 42 &&
          telemetry.direction == -1 && telemetry.step == 2 &&
          telemetry.state == 3 && telemetry.error == 1 && telemetry.reason == 7,
          "telemetry retains score, timing, direction, step, state, error and reason");
}

static void test_normal_af(void)
{
    struct smoothaf_normal_af af;
    enum smoothaf_normal_action action;
    int direction = 0;
    int step = 0;

    smoothaf_normal_af_init(&af, 1000, 1, 3);
    action = smoothaf_normal_af_begin(&af);
    CHECK(action == SMOOTHAF_NORMAL_STEP,
          "normal AF starts bounded acquisition");
    CHECK(smoothaf_normal_af_next_step(&af, &direction, &step),
          "normal AF emits acquisition movement");
    CHECK(direction == 1 && step == 3,
          "normal AF uses configured physical step");
    action = smoothaf_normal_af_on_score(&af, 1400);
    CHECK(action == SMOOTHAF_NORMAL_STEP,
          "normal AF continues acquisition after improvement");
    CHECK(af.best == 1400, "normal AF records acquisition best score");
    CHECK(af.acquisition_commands <= SMOOTHAF_NORMAL_ACQUISITION_COMMANDS,
          "normal AF acquisition has its own command budget");

    action = smoothaf_normal_af_on_score(&af, 1000);
    CHECK(action == SMOOTHAF_NORMAL_STEP,
          "normal AF enters bounded refinement after failed acquisition");
    CHECK(af.phase == SMOOTHAF_NORMAL_PHASE_REFINEMENT,
          "normal AF reports refinement phase");
    CHECK(af.refinement_commands == 0,
          "normal AF starts refinement with a separate budget");

    action = smoothaf_normal_af_on_score(&af, 1400);
    CHECK(action == SMOOTHAF_NORMAL_STEP || action == SMOOTHAF_NORMAL_CONFIRM,
          "normal AF refines around the best point");

    smoothaf_normal_af_init(&af, SMOOTHAF_MIN_SCORE - 1, 1, 3);
    action = smoothaf_normal_af_begin(&af);
    CHECK(action == SMOOTHAF_NORMAL_NO_CONFIDENCE,
          "normal AF rejects low-confidence acquisition");
    CHECK(af.commands == 0, "normal AF low-confidence start does not move");
}

static void test_adaptive_hill_climb(void)
{
    struct smoothaf_adaptive_policy policy;
    struct smoothaf_adaptive_hc hc;
    enum smoothaf_adaptive_action action;
    int direction = 0;
    int step = 0;

    smoothaf_adaptive_policy_default(&policy);
    CHECK(policy.min_step > 0, "adaptive policy has a positive minimum step");
    CHECK(policy.initial_step >= policy.min_step,
          "adaptive policy initial step is bounded");
    CHECK(policy.initial_step <= policy.max_step,
          "adaptive policy initial step fits the maximum");

    smoothaf_adaptive_hc_init(&hc, &policy, 1000, 1);
    action = smoothaf_adaptive_hc_begin(&hc);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive controller requests an abstract first step");
    CHECK(smoothaf_adaptive_hc_next_step(&hc, &direction, &step),
          "adaptive controller exposes the pending abstract step");
    CHECK(direction == 1 && step == policy.initial_step,
          "adaptive first step has configured direction and size");
    CHECK(hc.commands == 1, "adaptive first step is counted");
    CHECK(hc.pos == step, "adaptive position tracks the abstract step");

    /* Improvement, bounded overshoot, return, then two confirmations. */
    action = smoothaf_adaptive_hc_on_score(&hc, 1300);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive improvement keeps searching");
    CHECK(hc.best == 1300, "adaptive improvement updates the best score");
    CHECK(hc.best_pos == policy.initial_step,
          "adaptive improvement records the abstract position");
    action = smoothaf_adaptive_hc_on_score(&hc, 1000);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive overshoot requests a bounded return step");
    CHECK(hc.next_direction == -1, "adaptive overshoot reverses direction");
    action = smoothaf_adaptive_hc_on_score(&hc, 1300);
    CHECK(action == SMOOTHAF_ADAPTIVE_CONFIRM,
          "adaptive return requires confirmation");
    action = smoothaf_adaptive_hc_on_score(&hc, 1300);
    CHECK(action == SMOOTHAF_ADAPTIVE_LOCKED,
          "adaptive controller locks after confirmation");
    CHECK(hc.confirmations == policy.confirmations,
          "adaptive controller reaches configured confirmation count");

    /* A flat scene must terminate without claiming focus. */
    smoothaf_adaptive_hc_init(&hc, &policy, 1000, 1);
    action = smoothaf_adaptive_hc_begin(&hc);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive flat-scene probe starts with an abstract step");
    action = smoothaf_adaptive_hc_on_score(&hc, 1000);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive flat-scene probe returns to the best position");
    action = smoothaf_adaptive_hc_on_score(&hc, 1000);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive flat-scene probe checks the opposite side");
    action = smoothaf_adaptive_hc_on_score(&hc, 1000);
    CHECK(action == SMOOTHAF_ADAPTIVE_STEP,
          "adaptive flat-scene probe returns after the opposite side");
    action = smoothaf_adaptive_hc_on_score(&hc, 1000);
    CHECK(action == SMOOTHAF_ADAPTIVE_NO_PROGRESS,
          "adaptive flat scene stops on no progress");
    CHECK(hc.commands <= policy.max_commands,
          "adaptive flat-scene commands stay within policy");

    /* Low initial scores are rejected before any abstract movement. */
    smoothaf_adaptive_hc_init(&hc, &policy, policy.min_score - 1, 1);
    action = smoothaf_adaptive_hc_begin(&hc);
    CHECK(action == SMOOTHAF_ADAPTIVE_NO_CONFIDENCE,
          "adaptive low-confidence start does not move");
    CHECK(hc.commands == 0, "adaptive low-confidence start issues no step");
}

int main(void)
{
    test_auto_probe();
    test_uyvy_offsets();
    test_roi_and_geometry();
    test_metrics();
    test_enable_and_task_gate();
    test_measure_route();
    test_safe_mode_panic();
    test_measure_all_metrics();
    test_monitor_states_are_passive();
    test_hill_climb();
    test_adaptive_hill_climb();
    test_normal_af();
    test_telemetry();

    if (failures)
    {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }

    printf("All SmoothAF core tests passed\n");
    return 0;
}
