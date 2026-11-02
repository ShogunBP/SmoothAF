/*
 * SmoothAF 0.2.0 - hardware-independent core logic.
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

/* Safe-mode writes are lock-free so emergency stop remains available while
 * the focus task owns the semaphore or is blocked in lens_focus(). */
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

/* Movement still requires explicit Enable, no panic latch, and an idle task. */
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
static inline void smoothaf_measure_all(const uint8_t *base, int pitch,
                                        int *sad_out, int *laplacian_out,
                                        int *mean_out)
{
    int sad = 0;
    int laplacian = 0;
    int luma = 0;
    int samples = 0;
    int x;
    int y;

    for (y = 0; y < SMOOTHAF_ROI_SIZE - 2; y += SMOOTHAF_SAMPLE_STEP)
    {
        const uint8_t *row = base + y * pitch;
        const uint8_t *row2 = row + 2 * pitch;
        for (x = 0; x < SMOOTHAF_ROI_SIZE - 2; x += SMOOTHAF_SAMPLE_STEP)
        {
            int center = smoothaf_y(row, x);
            sad += smoothaf_abs(smoothaf_y(row, x + 2) - center);
            sad += smoothaf_abs(smoothaf_y(row2, x) - center);
            luma += center;
            samples++;
        }
    }

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
            laplacian += smoothaf_abs(lap);
        }
    }

    if (sad_out) *sad_out = sad;
    if (laplacian_out) *laplacian_out = laplacian;
    if (mean_out) *mean_out = samples ? luma / samples : 0;
}

static inline int smoothaf_measure_roi(const uint8_t *base, int pitch,
                                     int metric, int *mean_out)
{
    int sad;
    int laplacian;

    smoothaf_measure_all(base, pitch, &sad, &laplacian, mean_out);
    return metric == SMOOTHAF_METRIC_LAPLACIAN ? laplacian : sad;
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

/*
 * Conservative adaptive hill-climbing policy.
 *
 * This controller deliberately stops at an abstract step boundary.  The host
 * or camera adapter asks for a direction and step size with
 * smoothaf_adaptive_hc_next_step(); it owns the actual focus operation and
 * reports the resulting score with smoothaf_adaptive_hc_on_score().  No lens
 * or platform API is referenced here.
 */
#define SMOOTHAF_ADAPTIVE_MIN_STEP       1
#define SMOOTHAF_ADAPTIVE_INITIAL_STEP   1
#define SMOOTHAF_ADAPTIVE_MAX_STEP       3
#define SMOOTHAF_ADAPTIVE_MAX_COMMANDS   12
#define SMOOTHAF_ADAPTIVE_MAX_REVERSALS  3
#define SMOOTHAF_ADAPTIVE_CONFIRMATIONS  2
#define SMOOTHAF_ADAPTIVE_MIN_SCORE      SMOOTHAF_MIN_SCORE

enum smoothaf_adaptive_action
{
    SMOOTHAF_ADAPTIVE_STEP = 0,
    SMOOTHAF_ADAPTIVE_CONFIRM,
    SMOOTHAF_ADAPTIVE_LOCKED,
    SMOOTHAF_ADAPTIVE_NO_PROGRESS,
    SMOOTHAF_ADAPTIVE_NO_CONFIDENCE,
    SMOOTHAF_ADAPTIVE_LIMIT,
};

struct smoothaf_adaptive_policy
{
    int min_step;
    int initial_step;
    int max_step;
    int max_commands;
    int max_reversals;
    int confirmations;
    int min_score;
};

struct smoothaf_adaptive_hc
{
    struct smoothaf_adaptive_policy policy;
    int initial;
    int current;
    int best;
    int best_pos;
    int pos;
    int direction;
    int next_direction;
    int step;
    int next_step;
    int commands;
    int reversals;
    int confirmations;
    int pending;
    int phase;
};

enum smoothaf_adaptive_phase
{
    SMOOTHAF_ADAPTIVE_PHASE_SEARCH = 0,
    SMOOTHAF_ADAPTIVE_PHASE_RETURN,
    SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE,
    SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE_RETURN,
    SMOOTHAF_ADAPTIVE_PHASE_CONFIRM,
};

static inline void smoothaf_adaptive_policy_default(
        struct smoothaf_adaptive_policy *policy)
{
    policy->min_step = SMOOTHAF_ADAPTIVE_MIN_STEP;
    policy->initial_step = SMOOTHAF_ADAPTIVE_INITIAL_STEP;
    policy->max_step = SMOOTHAF_ADAPTIVE_MAX_STEP;
    policy->max_commands = SMOOTHAF_ADAPTIVE_MAX_COMMANDS;
    policy->max_reversals = SMOOTHAF_ADAPTIVE_MAX_REVERSALS;
    policy->confirmations = SMOOTHAF_ADAPTIVE_CONFIRMATIONS;
    policy->min_score = SMOOTHAF_ADAPTIVE_MIN_SCORE;
}

static inline int smoothaf_adaptive_clamp(const struct smoothaf_adaptive_policy *p,
                                          int value)
{
    if (value < p->min_step) return p->min_step;
    if (value > p->max_step) return p->max_step;
    return value;
}

static inline void smoothaf_adaptive_hc_init(
        struct smoothaf_adaptive_hc *hc,
        const struct smoothaf_adaptive_policy *policy,
        int initial,
        int direction)
{
    struct smoothaf_adaptive_policy defaults;

    if (!policy)
    {
        smoothaf_adaptive_policy_default(&defaults);
        policy = &defaults;
    }
    hc->policy = *policy;
    if (hc->policy.min_step < 1) hc->policy.min_step = 1;
    if (hc->policy.max_step < hc->policy.min_step)
        hc->policy.max_step = hc->policy.min_step;
    hc->policy.initial_step = smoothaf_adaptive_clamp(
        &hc->policy, hc->policy.initial_step);
    if (hc->policy.max_commands < 1) hc->policy.max_commands = 1;
    if (hc->policy.max_reversals < 0) hc->policy.max_reversals = 0;
    if (hc->policy.confirmations < 1) hc->policy.confirmations = 1;

    hc->initial = initial;
    hc->current = initial;
    hc->best = initial;
    hc->best_pos = 0;
    hc->pos = 0;
    hc->direction = direction < 0 ? -1 : 1;
    hc->next_direction = hc->direction;
    hc->step = hc->policy.initial_step;
    hc->next_step = hc->step;
    hc->commands = 0;
    hc->reversals = 0;
    hc->confirmations = 0;
    hc->pending = 0;
    hc->phase = SMOOTHAF_ADAPTIVE_PHASE_SEARCH;
}

static inline enum smoothaf_adaptive_action smoothaf_adaptive_hc_begin(
        struct smoothaf_adaptive_hc *hc)
{
    if (hc->initial < hc->policy.min_score)
    {
        return SMOOTHAF_ADAPTIVE_NO_CONFIDENCE;
    }
    if (hc->policy.max_commands <= 0)
    {
        return SMOOTHAF_ADAPTIVE_LIMIT;
    }
    hc->pending = 1;
    hc->next_direction = hc->direction;
    hc->next_step = hc->step;
    return SMOOTHAF_ADAPTIVE_STEP;
}

/* Reserve one abstract step and return its direction and bounded magnitude. */
static inline int smoothaf_adaptive_hc_next_step(
        struct smoothaf_adaptive_hc *hc, int *direction, int *step)
{
    if (!hc->pending || hc->commands >= hc->policy.max_commands)
    {
        hc->pending = 0;
        return 0;
    }
    hc->commands++;
    hc->direction = hc->next_direction < 0 ? -1 : 1;
    hc->step = smoothaf_adaptive_clamp(&hc->policy, hc->next_step);
    hc->pos += hc->direction * hc->step;
    if (direction) *direction = hc->direction;
    if (step) *step = hc->step;
    hc->pending = 0;
    return 1;
}

static inline enum smoothaf_adaptive_action smoothaf_adaptive_hc_schedule(
        struct smoothaf_adaptive_hc *hc, int direction, int step)
{
    if (hc->commands >= hc->policy.max_commands)
    {
        hc->pending = 0;
        return SMOOTHAF_ADAPTIVE_LIMIT;
    }
    hc->next_direction = direction < 0 ? -1 : 1;
    hc->next_step = smoothaf_adaptive_clamp(&hc->policy, step);
    hc->pending = 1;
    return SMOOTHAF_ADAPTIVE_STEP;
}

/*
 * Feed the score observed after the last abstract step.  Equal or small
 * changes never count as progress.  The first failed side is returned to the
 * origin, then the opposite side is probed once; a second failed return is
 * NO_PROGRESS.  Improvements may grow the step by one, but never beyond the
 * policy cap.  Every successful peak requires two confirmations.
 */
static inline enum smoothaf_adaptive_action smoothaf_adaptive_hc_on_score(
        struct smoothaf_adaptive_hc *hc, int score)
{
    int db = smoothaf_deadband(hc->best);

    hc->current = score;
    if (score < hc->policy.min_score)
    {
        return SMOOTHAF_ADAPTIVE_NO_CONFIDENCE;
    }

    if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_CONFIRM)
    {
        if (score + db < hc->best)
        {
            hc->confirmations = 0;
            hc->phase = SMOOTHAF_ADAPTIVE_PHASE_RETURN;
            return smoothaf_adaptive_hc_schedule(hc,
                                                  hc->best_pos < hc->pos ? -1 : 1,
                                                  hc->step);
        }
        hc->confirmations++;
        if (hc->confirmations >= hc->policy.confirmations)
        {
            hc->pending = 0;
            return SMOOTHAF_ADAPTIVE_LOCKED;
        }
        return SMOOTHAF_ADAPTIVE_CONFIRM;
    }

    if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_RETURN ||
        hc->phase == SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE_RETURN)
    {
        if (score > hc->best + db)
        {
            hc->best = score;
            hc->best_pos = hc->pos;
        }
        hc->confirmations = 1;
        if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_RETURN &&
            hc->best_pos == 0)
        {
            hc->phase = SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE;
            hc->reversals++;
            if (hc->reversals > hc->policy.max_reversals)
                return SMOOTHAF_ADAPTIVE_LIMIT;
            return smoothaf_adaptive_hc_schedule(hc, -hc->direction,
                                                  hc->policy.min_step);
        }
        if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE_RETURN)
        {
            hc->pending = 0;
            return SMOOTHAF_ADAPTIVE_NO_PROGRESS;
        }
        hc->phase = SMOOTHAF_ADAPTIVE_PHASE_CONFIRM;
        return SMOOTHAF_ADAPTIVE_CONFIRM;
    }

    if (score > hc->best + db)
    {
        hc->best = score;
        hc->best_pos = hc->pos;
        hc->confirmations = 0;
        if (hc->step < hc->policy.max_step) hc->step++;
        hc->phase = SMOOTHAF_ADAPTIVE_PHASE_SEARCH;
        return smoothaf_adaptive_hc_schedule(hc, hc->direction, hc->step);
    }

    /* A failed initial probe returns to the origin before probing opposite. */
    if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE)
    {
        hc->phase = SMOOTHAF_ADAPTIVE_PHASE_OPPOSITE_RETURN;
        hc->reversals++;
        if (hc->reversals > hc->policy.max_reversals)
            return SMOOTHAF_ADAPTIVE_LIMIT;
        return smoothaf_adaptive_hc_schedule(hc, -hc->direction,
                                              hc->policy.min_step);
    }

    if (hc->phase == SMOOTHAF_ADAPTIVE_PHASE_SEARCH)
    {
        hc->phase = SMOOTHAF_ADAPTIVE_PHASE_RETURN;
        hc->reversals++;
        if (hc->reversals > hc->policy.max_reversals)
            return SMOOTHAF_ADAPTIVE_LIMIT;
        return smoothaf_adaptive_hc_schedule(hc, -hc->direction,
                                              hc->policy.min_step);
    }

    hc->pending = 0;
    return SMOOTHAF_ADAPTIVE_NO_PROGRESS;
}

#define SMOOTHAF_NORMAL_ACQUISITION_COMMANDS  8
#define SMOOTHAF_NORMAL_REFINEMENT_COMMANDS   8
#define SMOOTHAF_NORMAL_MAX_COMMANDS          16
#define SMOOTHAF_NORMAL_MAX_REVERSALS         4

enum smoothaf_normal_phase
{
    SMOOTHAF_NORMAL_PHASE_ACQUISITION = 0,
    SMOOTHAF_NORMAL_PHASE_REFINEMENT,
    SMOOTHAF_NORMAL_PHASE_DONE,
};

enum smoothaf_normal_action
{
    SMOOTHAF_NORMAL_STEP = 0,
    SMOOTHAF_NORMAL_CONFIRM,
    SMOOTHAF_NORMAL_LOCKED,
    SMOOTHAF_NORMAL_LIMIT,
    SMOOTHAF_NORMAL_NO_CONFIDENCE,
};

struct smoothaf_normal_af
{
    int initial;
    int current;
    int best;
    int best_pos;
    int pos;
    int direction;
    int step;
    int commands;
    int reversals;
    int confirmations;
    int phase;
    int pending;
    int acquisition_commands;
    int refinement_commands;
    int improved;
};

static inline void smoothaf_normal_af_init(struct smoothaf_normal_af *af,
                                           int initial, int direction, int step)
{
    af->initial = initial;
    af->current = initial;
    af->best = initial;
    af->best_pos = 0;
    af->pos = 0;
    af->direction = direction < 0 ? -1 : 1;
    af->step = step < 1 ? 1 : step;
    af->commands = 0;
    af->reversals = 0;
    af->confirmations = 0;
    af->phase = SMOOTHAF_NORMAL_PHASE_ACQUISITION;
    af->pending = 0;
    af->acquisition_commands = 0;
    af->refinement_commands = 0;
    af->improved = 0;
}

static inline enum smoothaf_normal_action smoothaf_normal_af_begin(
        struct smoothaf_normal_af *af)
{
    if (af->initial < SMOOTHAF_MIN_SCORE)
        return SMOOTHAF_NORMAL_NO_CONFIDENCE;
    if (af->commands >= SMOOTHAF_NORMAL_MAX_COMMANDS)
        return SMOOTHAF_NORMAL_LIMIT;
    af->pending = 1;
    return SMOOTHAF_NORMAL_STEP;
}

static inline int smoothaf_normal_af_next_step(struct smoothaf_normal_af *af,
                                                int *direction, int *step)
{
    int phase_limit = af->phase == SMOOTHAF_NORMAL_PHASE_ACQUISITION
                    ? SMOOTHAF_NORMAL_ACQUISITION_COMMANDS
                    : SMOOTHAF_NORMAL_REFINEMENT_COMMANDS;
    int phase_commands = af->phase == SMOOTHAF_NORMAL_PHASE_ACQUISITION
                       ? af->acquisition_commands : af->refinement_commands;
    if (!af->pending || af->commands >= SMOOTHAF_NORMAL_MAX_COMMANDS ||
        phase_commands >= phase_limit)
    {
        af->pending = 0;
        return 0;
    }
    af->commands++;
    if (af->phase == SMOOTHAF_NORMAL_PHASE_ACQUISITION)
        af->acquisition_commands++;
    else
        af->refinement_commands++;
    af->pos += af->direction * af->step;
    af->pending = 0;
    if (direction) *direction = af->direction;
    if (step) *step = af->step;
    return 1;
}

static inline enum smoothaf_normal_action smoothaf_normal_af_schedule(
        struct smoothaf_normal_af *af, int direction)
{
    if (af->commands >= SMOOTHAF_NORMAL_MAX_COMMANDS ||
        af->reversals > SMOOTHAF_NORMAL_MAX_REVERSALS)
        return SMOOTHAF_NORMAL_LIMIT;
    af->direction = direction < 0 ? -1 : 1;
    af->pending = 1;
    return SMOOTHAF_NORMAL_STEP;
}

static inline enum smoothaf_normal_action smoothaf_normal_af_on_score(
        struct smoothaf_normal_af *af, int score)
{
    int db = smoothaf_deadband(af->best);
    af->current = score;
    if (score < SMOOTHAF_MIN_SCORE)
        return SMOOTHAF_NORMAL_NO_CONFIDENCE;

    if (score > af->best + db)
    {
        af->best = score;
        af->best_pos = af->pos;
        af->confirmations = 0;
        af->improved = 1;
        return smoothaf_normal_af_schedule(af, af->direction);
    }

    if (af->phase == SMOOTHAF_NORMAL_PHASE_ACQUISITION)
    {
        if (af->reversals == 0)
        {
            af->reversals++;
            af->phase = SMOOTHAF_NORMAL_PHASE_REFINEMENT;
            return smoothaf_normal_af_schedule(af, -af->direction);
        }
        af->phase = SMOOTHAF_NORMAL_PHASE_REFINEMENT;
        af->confirmations = 0;
    }

    if (af->pos != af->best_pos)
    {
        if (af->reversals >= SMOOTHAF_NORMAL_MAX_REVERSALS)
            return SMOOTHAF_NORMAL_LIMIT;
        af->reversals++;
        return smoothaf_normal_af_schedule(af,
                af->best_pos > af->pos ? 1 : -1);
    }

    af->confirmations++;
    if (af->confirmations >= 2)
    {
        af->phase = SMOOTHAF_NORMAL_PHASE_DONE;
        af->pending = 0;
        return af->improved ? SMOOTHAF_NORMAL_LOCKED
                            : SMOOTHAF_NORMAL_NO_CONFIDENCE;
    }
    return smoothaf_normal_af_schedule(af, -af->direction);
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
