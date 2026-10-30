/*
 * SmoothAF 0.1 - hardware-independent core logic.
 *
 * This header contains only plain C (no Magic Lantern or DryOS types) so the
 * YUV indexing, the sharpness metrics and the bounded hill-climbing controller
 * can be compiled and tested on the host.  smoothaf.c provides the hardware
 * bindings (LiveView buffer, lens commands, tasks and menu).
 *
 * UYVY layout (confirmed by src/imgconv.h and src/zebra.c):
 *
 *     U0 Y0 V0 Y1 U2 Y2 V2 Y3 ...
 *     byte: 0  1  2  3  4  5  6  7
 *
 * The luminance of pixel x is therefore at row[x * 2 + 1].
 */

#ifndef SMOOTHAF_CORE_H
#define SMOOTHAF_CORE_H

#include <stdint.h>

#define SMOOTHAF_ROI_SIZE       64
#define SMOOTHAF_SAMPLE_STEP    2

#define SMOOTHAF_METRIC_SAD        0
#define SMOOTHAF_METRIC_LAPLACIAN  1

/* Search limits.  Commands include every lens_focus() call, including the
 * controlled return to the best observed position. */
#define SMOOTHAF_MAX_COMMANDS   16
#define SMOOTHAF_MAX_REVERSALS  4

/* Experimental noise gates, not calibrated on hardware.
 * An improvement must exceed both an absolute floor and a relative fraction
 * of the current best score.  At a best score of 20000 the relative term is
 * 312 (~1.6%). */
#define SMOOTHAF_DEADBAND_MIN   24
#define SMOOTHAF_DEADBAND_DIV   64

/* Experimental confidence gates: very dark or almost textureless scenes are
 * reported as "No Confidence" instead of being searched. */
#define SMOOTHAF_MIN_LUMA       16
#define SMOOTHAF_MIN_SCORE      64

enum smoothaf_hc_action
{
    SMOOTHAF_HC_MOVE = 0,
    SMOOTHAF_HC_MEASURE,
    SMOOTHAF_HC_LOCKED,
    SMOOTHAF_HC_LIMIT,
    SMOOTHAF_HC_NO_CONFIDENCE,
};

enum smoothaf_result
{
    SMOOTHAF_RESULT_LOCKED = 0,
    SMOOTHAF_RESULT_LIMIT,
    SMOOTHAF_RESULT_NO_CONFIDENCE,
    SMOOTHAF_RESULT_CANCELLED,
    SMOOTHAF_RESULT_TIMEOUT,
    SMOOTHAF_RESULT_LENS_ERROR,
    SMOOTHAF_RESULT_MEASURE_ERROR,
    SMOOTHAF_RESULT_ERROR,
};

struct smoothaf_hc
{
    int initial;        /* score at relative position 0 */
    int current;        /* last measured score */
    int best;           /* best score observed */
    int best_pos;       /* relative position of the best score */
    int pos;            /* relative position of the last command */
    int dir;            /* -1 or +1 */
    int commands;       /* real movement commands issued */
    int reversals;      /* direction changes */
    int confirmations;  /* consecutive measurements at the best position */
    int local_max;      /* a peak or a verified two-sided maximum was found */
    int tried_opposite; /* the opposite direction was probed after a flat start */
    int worse_probes;   /* probes clearly below the best score */
    int flat_probes;    /* probes inside the deadband but not better */
};

static inline int smoothaf_abs(int v)
{
    return v < 0 ? -v : v;
}

/* Byte offset of Y inside an even-aligned UYVY row. */
static inline int smoothaf_y_offset(int x)
{
    return x * 2 + 1;
}

static inline int smoothaf_y(const uint8_t *row, int x)
{
    return row[smoothaf_y_offset(x)];
}

/* Keep the ROI start on an even pixel so that x = 0 is a Y byte. */
static inline int smoothaf_roi_left(int width)
{
    int left = (width - SMOOTHAF_ROI_SIZE) / 2;
    return left & ~1;
}

static inline int smoothaf_roi_top(int height)
{
    return (height - SMOOTHAF_ROI_SIZE) / 2;
}

/* Reject geometries that would make the ROI access unsafe. */
static inline int smoothaf_geometry_ok(int width, int height, int pitch)
{
    if (width < SMOOTHAF_ROI_SIZE || height < SMOOTHAF_ROI_SIZE) return 0;
    if (width > 8192 || height > 8192) return 0;
    if (pitch < width * 2) return 0;
    if (pitch & 1) return 0;
    return 1;
}

static inline int smoothaf_deadband(int best)
{
    int relative = best / SMOOTHAF_DEADBAND_DIV;
    return relative > SMOOTHAF_DEADBAND_MIN ? relative : SMOOTHAF_DEADBAND_MIN;
}

static inline int smoothaf_has_confidence(int score, int mean)
{
    return mean >= SMOOTHAF_MIN_LUMA && score >= SMOOTHAF_MIN_SCORE;
}

/* Safe-mode gate helpers.  PANIC writes are intentionally lock-free: they
 * must remain usable if the focus task owns or cannot acquire the semaphore. */
static inline void smoothaf_panic_latch(volatile int *enabled,
                                       volatile int *cancel,
                                       volatile int *panic)
{
    *panic = 1;
    *cancel = 1;
    *enabled = 0;
}

static inline void smoothaf_panic_rearm(volatile int *cancel,
                                       volatile int *panic)
{
    *panic = 0;
    *cancel = 0;
}

static inline int smoothaf_can_start(int enabled, int panic, int running)
{
    return enabled && !panic && !running;
}

/*
 * Compute the sharpness score of a 64x64 ROI and the mean luminance.
 * base must point to an even-aligned UYVY pixel (byte 0 of a UYVY group);
 * the caller must have validated the geometry with smoothaf_geometry_ok().
 * Integer only, no allocation.
 */
static inline int smoothaf_measure_roi(const uint8_t *base, int pitch,
                                     int metric, int *mean_out)
{
    int score = 0;
    int luma = 0;
    int samples = 0;
    int x;
    int y;

    if (metric != SMOOTHAF_METRIC_LAPLACIAN)
    {
        metric = SMOOTHAF_METRIC_SAD;
    }

    if (metric == SMOOTHAF_METRIC_LAPLACIAN)
    {
        for (y = 1; y < SMOOTHAF_ROI_SIZE - 1; y += SMOOTHAF_SAMPLE_STEP)
        {
            const uint8_t *row = base + y * pitch;
            const uint8_t *prev = row - pitch;
            const uint8_t *next = row + pitch;
            for (x = 1; x < SMOOTHAF_ROI_SIZE - 1; x += SMOOTHAF_SAMPLE_STEP)
            {
                int center = smoothaf_y(row, x);
                int lap = smoothaf_y(prev, x) + smoothaf_y(next, x) +
                          smoothaf_y(row, x - 1) + smoothaf_y(row, x + 1) -
                          4 * center;
                score += smoothaf_abs(lap);
                luma += center;
                samples++;
            }
        }
    }
    else
    {
        for (y = 0; y < SMOOTHAF_ROI_SIZE - 2; y += SMOOTHAF_SAMPLE_STEP)
        {
            const uint8_t *row = base + y * pitch;
            const uint8_t *row2 = row + 2 * pitch;
            for (x = 0; x < SMOOTHAF_ROI_SIZE - 2; x += SMOOTHAF_SAMPLE_STEP)
            {
                int center = smoothaf_y(row, x);
                score += smoothaf_abs(smoothaf_y(row, x + 2) - center);
                score += smoothaf_abs(smoothaf_y(row2, x) - center);
                luma += center;
                samples++;
            }
        }
    }

    if (mean_out)
    {
        *mean_out = samples ? luma / samples : 0;
    }
    return score;
}

static inline void smoothaf_hc_init(struct smoothaf_hc *hc, int initial, int dir)
{
    hc->initial = initial;
    hc->current = initial;
    hc->best = initial;
    hc->best_pos = 0;
    hc->pos = 0;
    hc->dir = dir < 0 ? -1 : 1;
    hc->commands = 0;
    hc->reversals = 0;
    hc->confirmations = 0;
    hc->local_max = 0;
    hc->tried_opposite = 0;
    hc->worse_probes = 0;
    hc->flat_probes = 0;
}

/* Reserve one real movement command and advance the relative position. */
static inline enum smoothaf_hc_action smoothaf_hc_next_move(struct smoothaf_hc *hc)
{
    if (hc->commands >= SMOOTHAF_MAX_COMMANDS)
    {
        return SMOOTHAF_HC_LIMIT;
    }
    hc->commands++;
    hc->pos += hc->dir;
    return SMOOTHAF_HC_MOVE;
}

static inline enum smoothaf_hc_action smoothaf_hc_confirm(struct smoothaf_hc *hc,
                                                      int score)
{
    if (score >= hc->best - smoothaf_deadband(hc->best))
    {
        hc->confirmations++;
        if (hc->confirmations >= 2)
        {
            return SMOOTHAF_HC_LOCKED;
        }
        return SMOOTHAF_HC_MEASURE;
    }
    return SMOOTHAF_HC_LIMIT;
}

/*
 * Feed a measured score to the controller.
 *
 * - An improvement above the deadband moves best_pos and keeps the direction.
 * - A non-improvement is treated as an overshoot: the direction is reversed and
 *   one command returns to best_pos (each command moves the same distance).
 * - At best_pos the score must be confirmed twice before LOCKED.
 * - If the first direction is flat/worse, the opposite direction is probed
 *   once.  If both sides are clearly worse, position 0 is accepted as a
 *   verified local maximum.  If either side is flat, the result is
 *   NO_CONFIDENCE.
 */
static inline enum smoothaf_hc_action smoothaf_hc_on_score(struct smoothaf_hc *hc,
                                                       int score)
{
    int db;

    hc->current = score;
    db = smoothaf_deadband(hc->best);

    if (score > hc->best + db)
    {
        hc->best = score;
        hc->best_pos = hc->pos;
        hc->local_max = 1;
        hc->confirmations = 0;
        return smoothaf_hc_next_move(hc);
    }

    if (hc->pos == hc->best_pos)
    {
        if (hc->local_max)
        {
            return smoothaf_hc_confirm(hc, score);
        }

        if (!hc->tried_opposite)
        {
            hc->tried_opposite = 1;
            return smoothaf_hc_next_move(hc);
        }

        if (hc->worse_probes >= 2 && hc->flat_probes == 0)
        {
            hc->local_max = 1;
            return smoothaf_hc_confirm(hc, score);
        }

        return SMOOTHAF_HC_NO_CONFIDENCE;
    }

    /* Overshoot: remember how the failed side looked, then return to best. */
    if (score + db < hc->best)
    {
        hc->worse_probes++;
    }
    else
    {
        hc->flat_probes++;
    }

    if (hc->reversals >= SMOOTHAF_MAX_REVERSALS)
    {
        return SMOOTHAF_HC_LIMIT;
    }

    hc->reversals++;
    hc->dir = -hc->dir;
    hc->confirmations = 0;
    return smoothaf_hc_next_move(hc);
}

typedef int (*smoothaf_hc_move_fn)(void *ctx, int dir);
typedef int (*smoothaf_hc_measure_fn)(void *ctx, int *score);
typedef int (*smoothaf_hc_abort_fn)(void *ctx); /* 0 = continue, else result */

/*
 * Drive the controller.  move() issues one lens command (already counted by
 * the controller); measure() waits for settling and returns a score; abort()
 * reports cancellation, timeout or a lost precondition.
 */
static inline enum smoothaf_result smoothaf_hc_run(
        struct smoothaf_hc *hc,
        int initial_score,
        int dir,
        smoothaf_hc_move_fn move,
        smoothaf_hc_measure_fn measure,
        smoothaf_hc_abort_fn abort,
        void *ctx)
{
    enum smoothaf_hc_action action;
    int score = initial_score;
    int stop;

    smoothaf_hc_init(hc, initial_score, dir);
    action = smoothaf_hc_next_move(hc);

    for (;;)
    {
        stop = abort ? abort(ctx) : 0;
        if (stop)
        {
            return (enum smoothaf_result)stop;
        }

        if (action == SMOOTHAF_HC_MOVE)
        {
            if (!move(ctx, hc->dir))
            {
                return SMOOTHAF_RESULT_LENS_ERROR;
            }

            /* A synchronous lens command may have blocked for a while.
             * Observe Stop/timeouts before any further movement. */
            stop = abort ? abort(ctx) : 0;
            if (stop)
            {
                return (enum smoothaf_result)stop;
            }
        }

        if (!measure(ctx, &score))
        {
            return SMOOTHAF_RESULT_MEASURE_ERROR;
        }

        /* A measurement may include settling or frame processing.  Recheck
         * cancellation and timeout before allowing it to confirm LOCKED or
         * schedule another movement. */
        stop = abort ? abort(ctx) : 0;
        if (stop)
        {
            return (enum smoothaf_result)stop;
        }

        action = smoothaf_hc_on_score(hc, score);

        if (action == SMOOTHAF_HC_LOCKED)
        {
            return SMOOTHAF_RESULT_LOCKED;
        }
        if (action == SMOOTHAF_HC_LIMIT)
        {
            return SMOOTHAF_RESULT_LIMIT;
        }
        if (action == SMOOTHAF_HC_NO_CONFIDENCE)
        {
            return SMOOTHAF_RESULT_NO_CONFIDENCE;
        }
    }
}

#endif /* SMOOTHAF_CORE_H */
