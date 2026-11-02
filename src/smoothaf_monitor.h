#ifndef SMOOTHAF_MONITOR_H
#define SMOOTHAF_MONITOR_H

#include <stdint.h>

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

#define SMOOTHAF_VLOG_AF_OFF       0
#define SMOOTHAF_VLOG_AF_DRY_RUN   1
#define SMOOTHAF_VLOG_AF_ACTIVE    2

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
#define SMOOTHAF_VLOG_AF_SETTLE_MS      120

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
        default:                                      return "NONE";
    }
}

/* Arming is manual, but the monitor task itself is allowed to be running:
 * that task is the consumer of the episode, not a conflicting focus task. */
static inline int smoothaf_vlog_af_can_arm(int mode, int monitor_enabled,
                                           int task_running,
                                           int monitor_task_requested)
{
    if (mode == SMOOTHAF_VLOG_AF_OFF) return 0;
    if (!monitor_enabled) return 0;
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
    int awaiting_result;
    int dry_run_requests;
    int episode_result;
    int initial_score;
    int previous_score;
    int post_move_score;
    int score_delta;
    int best_position;
    int current_position;
    uint32_t started_ms;
    int uncertain;
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
    af->awaiting_result = 0;
    af->dry_run_requests = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    af->initial_score = 0;
    af->previous_score = 0;
    af->post_move_score = 0;
    af->score_delta = 0;
    af->best_position = 0;
    af->current_position = 0;
    af->last_direction = 1;
    af->started_ms = 0;
    af->uncertain = 0;
}

static inline void smoothaf_vlog_af_new_episode(struct smoothaf_vlog_af *af,
                                                int mode, int direction,
                                                int baseline)
{
    af->generation++;
    af->mode = mode;
    af->state = SMOOTHAF_VLOG_AF_WAITING;
    af->armed = 1;
    af->direction = direction < 0 ? -1 : 1;
    af->last_direction = af->direction;
    af->baseline = baseline;
    af->best = baseline;
    af->commands = 0;
    af->reversals = 0;
    af->settle_until_ms = 0;
    af->cooldown_until_ms = 0;
    af->awaiting_result = 0;
    af->dry_run_requests = 0;
    af->episode_result = SMOOTHAF_VLOG_AF_RESULT_NONE;
    af->initial_score = baseline;
    af->previous_score = baseline;
    af->post_move_score = 0;
    af->score_delta = 0;
    af->best_position = 0;
    af->current_position = 0;
    af->last_direction = af->direction;
    af->started_ms = 0;
    af->uncertain = 0;
}

static inline void smoothaf_vlog_af_stop(struct smoothaf_vlog_af *af)
{
    af->armed = 0;
    af->awaiting_result = 0;
    if (af->episode_result == SMOOTHAF_VLOG_AF_RESULT_NONE ||
        af->episode_result == SMOOTHAF_VLOG_AF_RESULT_ARMED)
        af->episode_result = af->commands
                         ? SMOOTHAF_VLOG_AF_RESULT_CANCELLED
                         : SMOOTHAF_VLOG_AF_RESULT_NO_MOVEMENT;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
}

static inline void smoothaf_vlog_af_record_command(
    struct smoothaf_vlog_af *af, uint32_t now)
{
    if (af->commands != UINT32_MAX) af->commands++;
    if (af->session_commands != UINT32_MAX) af->session_commands++;
    if (!af->started_ms) af->started_ms = now;
    af->awaiting_result = 1;
    af->state = SMOOTHAF_VLOG_AF_SETTLING;
    af->settle_until_ms = now + SMOOTHAF_VLOG_AF_SETTLE_MS;
    af->last_direction = af->direction;
}

static inline int smoothaf_vlog_af_can_command(
    struct smoothaf_vlog_af *af, int enabled, uint32_t now)
{
    if (!af->armed || af->mode == SMOOTHAF_VLOG_AF_OFF) return 0;
    if (af->mode == SMOOTHAF_VLOG_AF_ACTIVE && !enabled) return 0;
    if (af->awaiting_result) return 0;
    if (af->state == SMOOTHAF_VLOG_AF_COOLDOWN &&
        (int32_t)(now - af->cooldown_until_ms) >= 0)
        af->state = SMOOTHAF_VLOG_AF_PROBING;
    if (af->state != SMOOTHAF_VLOG_AF_PROBING) return 0;
    if (af->commands >= SMOOTHAF_VLOG_AF_MAX_COMMANDS) return 0;
    if (af->session_commands >= SMOOTHAF_VLOG_AF_MAX_COMMANDS) return 0;
    if (af->cooldown_until_ms && (int32_t)(now - af->cooldown_until_ms) < 0)
        return 0;
    return 1;
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
    af->post_move_score = score;
    af->score_delta = delta;
    af->awaiting_result = 0;
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
        if (af->commands >= SMOOTHAF_VLOG_AF_MAX_COMMANDS)
        {
            af->armed = 0;
            af->episode_result = SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT;
        }
        return 1;
    }
    if (delta < -deadband)
    {
        af->episode_result = SMOOTHAF_VLOG_AF_RESULT_WORSE;
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
    af->armed = 0;
    af->state = SMOOTHAF_VLOG_AF_COMPLETE;
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
