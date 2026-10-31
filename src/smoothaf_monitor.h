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
