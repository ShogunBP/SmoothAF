#ifndef SMOOTHAF_RECOVERY_H
#define SMOOTHAF_RECOVERY_H

#include "smoothaf_core.h"

/* Experimental, bounded far-focus acquisition.  This is deliberately a
 * camera-independent state machine: the camera adapter only owns the actual
 * lens command and image sample.  It never sweeps without a command budget. */
#define SMOOTHAF_RECOVERY_COARSE_STEP 3
#define SMOOTHAF_RECOVERY_FINE_STEP   1
#define SMOOTHAF_RECOVERY_MAX_COMMANDS 8
#define SMOOTHAF_RECOVERY_TRIGGER_SCORE (SMOOTHAF_MIN_SCORE * 4)

enum smoothaf_recovery_phase
{
    SMOOTHAF_RECOVERY_IDLE = 0,
    SMOOTHAF_RECOVERY_COARSE,
    SMOOTHAF_RECOVERY_BRACKET,
    SMOOTHAF_RECOVERY_FINE,
    SMOOTHAF_RECOVERY_RETURN,
    SMOOTHAF_RECOVERY_COMPLETE,
    SMOOTHAF_RECOVERY_ABORTED,
};

enum smoothaf_recovery_result
{
    SMOOTHAF_RECOVERY_NONE = 0,
    SMOOTHAF_RECOVERY_BEST_RECOVERED,
    SMOOTHAF_RECOVERY_BEST_NOT_RECOVERED,
    SMOOTHAF_RECOVERY_LIMIT,
    SMOOTHAF_RECOVERY_CANCELLED,
};

static inline const char *smoothaf_recovery_phase_name(int phase)
{
    switch (phase)
    {
        case SMOOTHAF_RECOVERY_COARSE: return "COARSE";
        case SMOOTHAF_RECOVERY_BRACKET: return "BRACKET";
        case SMOOTHAF_RECOVERY_FINE: return "FINE";
        case SMOOTHAF_RECOVERY_RETURN: return "RETURN";
        case SMOOTHAF_RECOVERY_COMPLETE: return "COMPLETE";
        case SMOOTHAF_RECOVERY_ABORTED: return "ABORTED";
        default: return "IDLE";
    }
}

static inline const char *smoothaf_recovery_result_name(int result)
{
    switch (result)
    {
        case SMOOTHAF_RECOVERY_BEST_RECOVERED: return "RECOVERED";
        case SMOOTHAF_RECOVERY_BEST_NOT_RECOVERED: return "NOT_RECOVERED";
        case SMOOTHAF_RECOVERY_LIMIT: return "LIMIT";
        case SMOOTHAF_RECOVERY_CANCELLED: return "CANCELLED";
        default: return "NONE";
    }
}

struct smoothaf_recovery
{
    int phase;
    int result;
    int initial_score;
    int best_score;
    int best_position;
    int position;
    int pending_position;
    int direction;
    int next_direction;
    int next_step;
    int target;
    int sequence;
    int commands;
};

static inline void smoothaf_recovery_init(struct smoothaf_recovery *recovery)
{
    recovery->phase = SMOOTHAF_RECOVERY_IDLE;
    recovery->result = SMOOTHAF_RECOVERY_NONE;
    recovery->initial_score = 0;
    recovery->best_score = 0;
    recovery->best_position = 0;
    recovery->position = 0;
    recovery->pending_position = 0;
    recovery->direction = 1;
    recovery->next_direction = 1;
    recovery->next_step = SMOOTHAF_RECOVERY_COARSE_STEP;
    recovery->target = 0;
    recovery->sequence = 0;
    recovery->commands = 0;
}

static inline int smoothaf_recovery_should_start(int enabled, int score,
                                                  int movie_active,
                                                  int explicitly_enabled)
{
    /* Movie is disabled by default; an explicit opt-in is required even when
     * the monitor reports a low-confidence/far-focus score. */
    return enabled && (!movie_active || explicitly_enabled) && score >= 0 &&
           score < SMOOTHAF_RECOVERY_TRIGGER_SCORE;
}

static inline void smoothaf_recovery_begin(struct smoothaf_recovery *recovery,
                                           int initial_score, int direction)
{
    smoothaf_recovery_init(recovery);
    recovery->phase = SMOOTHAF_RECOVERY_COARSE;
    recovery->initial_score = initial_score;
    recovery->best_score = initial_score;
    recovery->direction = direction < 0 ? -1 : 1;
    recovery->next_direction = recovery->direction;
    recovery->next_step = SMOOTHAF_RECOVERY_COARSE_STEP;
    recovery->target = recovery->direction * SMOOTHAF_RECOVERY_COARSE_STEP;
}

static inline int smoothaf_recovery_next(struct smoothaf_recovery *recovery,
                                         int *direction, int *step)
{
    int delta;
    if (recovery->phase == SMOOTHAF_RECOVERY_COMPLETE ||
        recovery->phase == SMOOTHAF_RECOVERY_ABORTED ||
        recovery->commands >= SMOOTHAF_RECOVERY_MAX_COMMANDS)
    {
        if (recovery->commands >= SMOOTHAF_RECOVERY_MAX_COMMANDS &&
            recovery->phase != SMOOTHAF_RECOVERY_COMPLETE)
        {
            recovery->phase = SMOOTHAF_RECOVERY_ABORTED;
            recovery->result = SMOOTHAF_RECOVERY_LIMIT;
        }
        return 0;
    }
    delta = recovery->target - recovery->position;
    if (delta == 0)
    {
        if (recovery->phase == SMOOTHAF_RECOVERY_RETURN)
        {
            int db = smoothaf_deadband(recovery->best_score);
            recovery->phase = SMOOTHAF_RECOVERY_COMPLETE;
            recovery->result = recovery->best_score >
                               recovery->initial_score + db
                             ? SMOOTHAF_RECOVERY_BEST_RECOVERED
                             : SMOOTHAF_RECOVERY_BEST_NOT_RECOVERED;
        }
        return 0;
    }
    if (delta > 0)
    {
        recovery->next_direction = 1;
        recovery->next_step = delta >
                              (recovery->phase == SMOOTHAF_RECOVERY_FINE
                               ? SMOOTHAF_RECOVERY_FINE_STEP
                               : SMOOTHAF_RECOVERY_COARSE_STEP)
                            ? (recovery->phase == SMOOTHAF_RECOVERY_FINE
                               ? SMOOTHAF_RECOVERY_FINE_STEP
                               : SMOOTHAF_RECOVERY_COARSE_STEP) : delta;
    }
    else
    {
        recovery->next_direction = -1;
        recovery->next_step = -delta >
                              (recovery->phase == SMOOTHAF_RECOVERY_FINE
                               ? SMOOTHAF_RECOVERY_FINE_STEP
                               : SMOOTHAF_RECOVERY_COARSE_STEP)
                            ? (recovery->phase == SMOOTHAF_RECOVERY_FINE
                               ? SMOOTHAF_RECOVERY_FINE_STEP
                               : SMOOTHAF_RECOVERY_COARSE_STEP) : -delta;
    }
    if (direction) *direction = recovery->next_direction;
    if (step) *step = recovery->next_step;
    recovery->pending_position = recovery->position +
                                 recovery->next_direction * recovery->next_step;
    recovery->commands++;
    return 1;
}

static inline void smoothaf_recovery_commit_move(
    struct smoothaf_recovery *recovery)
{
    recovery->position = recovery->pending_position;
}

static inline int smoothaf_recovery_on_score(struct smoothaf_recovery *recovery,
                                             int score)
{
    int db = smoothaf_deadband(recovery->best_score);
    if (score > recovery->best_score + db)
    {
        recovery->best_score = score;
        recovery->best_position = recovery->position;
    }
    if (recovery->phase == SMOOTHAF_RECOVERY_COARSE)
    {
        recovery->sequence++;
        recovery->target = 0;
        recovery->phase = SMOOTHAF_RECOVERY_BRACKET;
        return 1;
    }
    if (recovery->phase == SMOOTHAF_RECOVERY_BRACKET)
    {
        recovery->sequence++;
        if (recovery->sequence == 2)
        {
            recovery->target = -recovery->direction * SMOOTHAF_RECOVERY_COARSE_STEP;
            return 1;
        }
        recovery->target = recovery->position + recovery->direction;
        recovery->phase = SMOOTHAF_RECOVERY_FINE;
        recovery->next_step = SMOOTHAF_RECOVERY_FINE_STEP;
        return 1;
    }
    if (recovery->phase == SMOOTHAF_RECOVERY_FINE)
    {
        recovery->sequence++;
        if (recovery->sequence == 4)
        {
            recovery->target = recovery->best_position;
            recovery->phase = SMOOTHAF_RECOVERY_RETURN;
            return 1;
        }
        recovery->target = recovery->best_position;
        recovery->phase = SMOOTHAF_RECOVERY_RETURN;
        return 1;
    }
    if (recovery->phase == SMOOTHAF_RECOVERY_RETURN)
    {
        if (recovery->position != recovery->best_position)
        {
            recovery->target = recovery->best_position;
            return 1;
        }
        recovery->phase = SMOOTHAF_RECOVERY_COMPLETE;
        recovery->result = recovery->best_score >
                           recovery->initial_score + db
                         ? SMOOTHAF_RECOVERY_BEST_RECOVERED
                         : SMOOTHAF_RECOVERY_BEST_NOT_RECOVERED;
        return 0;
    }
    recovery->phase = SMOOTHAF_RECOVERY_ABORTED;
    recovery->result = SMOOTHAF_RECOVERY_LIMIT;
    return 0;
}

static inline void smoothaf_recovery_cancel(struct smoothaf_recovery *recovery)
{
    recovery->phase = SMOOTHAF_RECOVERY_ABORTED;
    recovery->result = SMOOTHAF_RECOVERY_CANCELLED;
}

#endif
