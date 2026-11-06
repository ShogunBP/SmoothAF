#ifndef SMOOTHAF_D1_H
#define SMOOTHAF_D1_H

/*
 * SmoothAF B2-D1 stationary diagnostic seam.
 *
 * This module is deliberately an adapter seam, not a second monitor or a
 * movement path.  The caller supplies the existing control exclusion
 * operation (smoothaf_lock()/smoothaf_unlock() in production, a host
 * equivalent in tests).  It owns only bounded D1 state and never performs
 * card I/O, focus-movement API calls, or image acquisition itself.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SMOOTHAF_D1_MAX_SAMPLES 30u
#define SMOOTHAF_D1_TEXT_MAX 64u

#define SMOOTHAF_D1_OWNER_NONE 0u
#define SMOOTHAF_D1_OWNER_MONITOR 1u
#define SMOOTHAF_D1_OWNER_FOCUS_TEST 2u
#define SMOOTHAF_D1_OWNER_MEASUREMENT 3u
#define SMOOTHAF_D1_OWNER_D1 4u

#define SMOOTHAF_D1_REJECT_NONE 0
#define SMOOTHAF_D1_REJECT_LOCK 1
#define SMOOTHAF_D1_REJECT_STOP_PANIC 2
#define SMOOTHAF_D1_REJECT_MONITOR 3
#define SMOOTHAF_D1_REJECT_EPISODE 4
#define SMOOTHAF_D1_REJECT_FOCUS_TEST 5
#define SMOOTHAF_D1_REJECT_MEASUREMENT 6
#define SMOOTHAF_D1_REJECT_BUSY 7
#define SMOOTHAF_D1_REJECT_NOT_ACTIVE 8
#define SMOOTHAF_D1_REJECT_FOREIGN_PRODUCER 9
#define SMOOTHAF_D1_REJECT_LIMIT 10
#define SMOOTHAF_D1_REJECT_INVALID 11

#define SMOOTHAF_D1_STATUS_IDLE 0
#define SMOOTHAF_D1_STATUS_ACTIVE 1
#define SMOOTHAF_D1_STATUS_NORMAL 2
#define SMOOTHAF_D1_STATUS_CANCELLED 3
#define SMOOTHAF_D1_STATUS_ERROR 4
#define SMOOTHAF_D1_STATUS_CONTEXT_INVALID 5

#define SMOOTHAF_D1_PUBLISH_OK 0
#define SMOOTHAF_D1_PUBLISH_DROP 1
#define SMOOTHAF_D1_PUBLISH_ERROR 2
#define SMOOTHAF_D1_PUBLISH_PARTIAL 3

typedef int (*smoothaf_d1_lock_fn)(void *context);
typedef void (*smoothaf_d1_unlock_fn)(void *context);

typedef int (*smoothaf_d1_publish_start_fn)(const void *series, void *context);
typedef int (*smoothaf_d1_publish_context_fn)(const void *series, void *context);
typedef int (*smoothaf_d1_publish_sample_fn)(const void *sample, void *context);
typedef int (*smoothaf_d1_publish_end_fn)(const void *series, void *context);

struct smoothaf_d1_gate_state
{
    volatile uint32_t owner;
    volatile int monitor_active;
    volatile int episode_active;
    volatile int focus_test_active;
    volatile int stop_requested;
    volatile int panic;
    uint32_t generation;
};

struct smoothaf_d1_gate
{
    struct smoothaf_d1_gate_state state;
    smoothaf_d1_lock_fn lock;
    smoothaf_d1_unlock_fn unlock;
    void *lock_context;
};

struct smoothaf_d1_context
{
    char scene[SMOOTHAF_D1_TEXT_MAX];
    char distance[SMOOTHAF_D1_TEXT_MAX];
    char lighting[SMOOTHAF_D1_TEXT_MAX];
    char exposure[SMOOTHAF_D1_TEXT_MAX];
    char roi[SMOOTHAF_D1_TEXT_MAX];
    char metric[SMOOTHAF_D1_TEXT_MAX];
    char focus_state[SMOOTHAF_D1_TEXT_MAX];
    char capture_mode[SMOOTHAF_D1_TEXT_MAX];
    char recording[SMOOTHAF_D1_TEXT_MAX];
    char cadence[SMOOTHAF_D1_TEXT_MAX];
    char build_id[SMOOTHAF_D1_TEXT_MAX];
};

struct smoothaf_d1_sample
{
    uint32_t sequence;
    uint32_t timestamp_ms;
    int32_t score;
    int32_t mean;
    int32_t sad;
    int32_t laplacian;
    uint32_t valid;
    uint32_t confident;
    uint32_t producer_id;
};

struct smoothaf_d1_series
{
    struct smoothaf_d1_gate *gate;
    struct smoothaf_d1_context context;
    uint32_t id;
    volatile uint32_t started_ms;
    volatile uint32_t ended_ms;
    volatile uint32_t requested;
    volatile uint32_t produced;
    volatile uint32_t start_attempts;
    volatile uint32_t start_accepted;
    volatile uint32_t start_drops;
    volatile uint32_t start_errors;
    volatile uint32_t context_attempts;
    volatile uint32_t context_accepted;
    volatile uint32_t context_drops;
    volatile uint32_t context_errors;
    volatile uint32_t context_aux_attempts;
    volatile uint32_t context_aux_accepted;
    volatile uint32_t context_aux_drops;
    volatile uint32_t context_aux_errors;
    volatile uint32_t sample_attempts;
    volatile uint32_t sample_accepted;
    volatile uint32_t sample_drops;
    volatile uint32_t sample_errors;
    volatile uint32_t end_attempts;
    volatile uint32_t end_accepted;
    volatile uint32_t end_drops;
    volatile uint32_t end_errors;
    int rec_class_initial;
    int rec_class_current;
    volatile uint32_t published;
    volatile uint32_t next_sequence;
    volatile uint32_t dropped_count;
    volatile uint32_t error_count;
    volatile uint32_t partial_count;
    volatile uint32_t sequence_gaps;
    volatile uint32_t foreign_producer_count;
    volatile uint32_t owner;
    volatile uint32_t generation;
    volatile int status;
    volatile int end_published;
    volatile int integrity;
    volatile int last_reject;
    struct smoothaf_d1_sample samples[SMOOTHAF_D1_MAX_SAMPLES];
    smoothaf_d1_publish_start_fn publish_start;
    smoothaf_d1_publish_context_fn publish_context_event;
    smoothaf_d1_publish_context_fn publish_context_aux_event;
    smoothaf_d1_publish_sample_fn publish_sample;
    smoothaf_d1_publish_end_fn publish_end;
    void *publish_context;
};

static inline void smoothaf_d1_context_clear(struct smoothaf_d1_context *context)
{
    if (context) memset(context, 0, sizeof(*context));
}

static inline void smoothaf_d1_copy_text(char *out, size_t out_size,
                                         const char *value)
{
    size_t length;
    if (!out || !out_size) return;
    if (!value) value = "";
    length = strlen(value);
    if (length >= out_size) length = out_size - 1u;
    memcpy(out, value, length);
    out[length] = '\0';
}

static inline int smoothaf_d1_lock(struct smoothaf_d1_gate *gate)
{
    return gate && gate->lock && gate->unlock &&
           gate->lock(gate->lock_context);
}

static inline void smoothaf_d1_unlock(struct smoothaf_d1_gate *gate)
{
    if (gate && gate->unlock) gate->unlock(gate->lock_context);
}

static inline void smoothaf_d1_gate_init(struct smoothaf_d1_gate *gate,
                                         smoothaf_d1_lock_fn lock,
                                         smoothaf_d1_unlock_fn unlock,
                                         void *lock_context)
{
    if (!gate) return;
    memset(gate, 0, sizeof(*gate));
    gate->lock = lock;
    gate->unlock = unlock;
    gate->lock_context = lock_context;
}

static inline int smoothaf_d1_gate_reject(int *reason, int value)
{
    if (reason) *reason = value;
    return 0;
}

static inline int smoothaf_d1_gate_start_monitor(struct smoothaf_d1_gate *gate,
                                                 int episode_active,
                                                 int *reason)
{
    if (!smoothaf_d1_lock(gate)) return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_LOCK);
    if (gate->state.stop_requested || gate->state.panic) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_STOP_PANIC);
    }
    if (gate->state.owner != SMOOTHAF_D1_OWNER_NONE) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason,
            gate->state.owner == SMOOTHAF_D1_OWNER_D1
                ? SMOOTHAF_D1_REJECT_BUSY : SMOOTHAF_D1_REJECT_MONITOR);
    }
    if (gate->state.focus_test_active || gate->state.episode_active) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_BUSY);
    }
    gate->state.owner = SMOOTHAF_D1_OWNER_MONITOR;
    gate->state.monitor_active = 1;
    gate->state.episode_active = episode_active ? 1 : 0;
    gate->state.generation++;
    smoothaf_d1_unlock(gate);
    if (reason) *reason = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline int smoothaf_d1_gate_start_focus_test(struct smoothaf_d1_gate *gate,
                                                    int *reason)
{
    if (!smoothaf_d1_lock(gate)) return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_LOCK);
    if (gate->state.stop_requested || gate->state.panic) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_STOP_PANIC);
    }
    if (gate->state.owner != SMOOTHAF_D1_OWNER_NONE ||
        gate->state.monitor_active || gate->state.episode_active) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason,
            gate->state.owner == SMOOTHAF_D1_OWNER_D1
                ? SMOOTHAF_D1_REJECT_BUSY : SMOOTHAF_D1_REJECT_MONITOR);
    }
    gate->state.owner = SMOOTHAF_D1_OWNER_FOCUS_TEST;
    gate->state.focus_test_active = 1;
    gate->state.generation++;
    smoothaf_d1_unlock(gate);
    if (reason) *reason = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline int smoothaf_d1_gate_start_measurement(struct smoothaf_d1_gate *gate,
                                                     uint32_t producer_id,
                                                     int *reason)
{
    if (!smoothaf_d1_lock(gate)) return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_LOCK);
    if (gate->state.stop_requested || gate->state.panic) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_STOP_PANIC);
    }
    if (gate->state.owner != SMOOTHAF_D1_OWNER_NONE) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_BUSY);
    }
    if (producer_id == SMOOTHAF_D1_OWNER_D1) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_INVALID);
    }
    gate->state.owner = SMOOTHAF_D1_OWNER_MEASUREMENT;
    gate->state.generation++;
    smoothaf_d1_unlock(gate);
    if (reason) *reason = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline int smoothaf_d1_gate_owns_generation(struct smoothaf_d1_gate *gate,
                                                    uint32_t generation)
{
    int owns;
    if (!smoothaf_d1_lock(gate)) return 0;
    owns = gate->state.owner == SMOOTHAF_D1_OWNER_D1 &&
           gate->state.generation == generation &&
           !gate->state.stop_requested && !gate->state.panic;
    smoothaf_d1_unlock(gate);
    return owns;
}

static inline int smoothaf_d1_gate_owns_d1(struct smoothaf_d1_gate *gate)
{
    return smoothaf_d1_gate_owns_generation(gate, gate ? gate->state.generation : 0);
}

static inline int smoothaf_d1_gate_acquire(struct smoothaf_d1_gate *gate,
                                           int *reason)
{
    if (!smoothaf_d1_lock(gate)) return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_LOCK);
    if (gate->state.stop_requested || gate->state.panic) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_STOP_PANIC);
    }
    if (gate->state.monitor_active) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_MONITOR);
    }
    if (gate->state.episode_active) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_EPISODE);
    }
    if (gate->state.focus_test_active) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_FOCUS_TEST);
    }
    if (gate->state.owner != SMOOTHAF_D1_OWNER_NONE) {
        smoothaf_d1_unlock(gate);
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_BUSY);
    }
    gate->state.owner = SMOOTHAF_D1_OWNER_D1;
    gate->state.generation++;
    smoothaf_d1_unlock(gate);
    if (reason) *reason = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline void smoothaf_d1_gate_end(struct smoothaf_d1_gate *gate,
                                        uint32_t owner)
{
    if (!smoothaf_d1_lock(gate)) return;
    if (gate->state.owner == owner) {
        gate->state.owner = SMOOTHAF_D1_OWNER_NONE;
        if (owner == SMOOTHAF_D1_OWNER_MONITOR) {
            gate->state.monitor_active = 0;
            gate->state.episode_active = 0;
        }
        if (owner == SMOOTHAF_D1_OWNER_FOCUS_TEST)
            gate->state.focus_test_active = 0;
        gate->state.generation++;
    }
    smoothaf_d1_unlock(gate);
}

static inline void smoothaf_d1_gate_set_episode(struct smoothaf_d1_gate *gate,
                                                int active)
{
    if (!smoothaf_d1_lock(gate)) return;
    if (gate->state.owner == SMOOTHAF_D1_OWNER_MONITOR ||
        gate->state.owner == SMOOTHAF_D1_OWNER_NONE)
        gate->state.episode_active = active ? 1 : 0;
    smoothaf_d1_unlock(gate);
}

static inline void smoothaf_d1_gate_signal_stop(struct smoothaf_d1_gate *gate,
                                                int panic)
{
    if (!smoothaf_d1_lock(gate)) return;
    gate->state.stop_requested = 1;
    if (panic) gate->state.panic = 1;
    smoothaf_d1_unlock(gate);
}

/* PANIC may run without the semaphore because a focus task can own it or be
 * inside a synchronous lens primitive.  Only cancellation bits are latched
 * urgently; normal ownership transitions still use the shared lock. */
static inline void smoothaf_d1_gate_signal_stop_urgent(
    struct smoothaf_d1_gate *gate, int panic)
{
    if (!gate) return;
    gate->state.stop_requested = 1;
    if (panic) gate->state.panic = 1;
}

static inline int smoothaf_d1_gate_cancelled(struct smoothaf_d1_gate *gate)
{
    int cancelled;
    if (!smoothaf_d1_lock(gate)) return 1;
    cancelled = gate->state.stop_requested || gate->state.panic;
    smoothaf_d1_unlock(gate);
    return cancelled;
}

static inline void smoothaf_d1_gate_clear_stop(struct smoothaf_d1_gate *gate)
{
    if (!smoothaf_d1_lock(gate)) return;
    gate->state.stop_requested = 0;
    gate->state.panic = 0;
    smoothaf_d1_unlock(gate);
}

static inline void smoothaf_d1_series_zero(struct smoothaf_d1_series *series)
{
    if (series) memset(series, 0, sizeof(*series));
}

static inline void smoothaf_d1_count_independent_publish(
    struct smoothaf_d1_series *series, int result,
    volatile uint32_t *attempts, volatile uint32_t *accepted,
    volatile uint32_t *drops, volatile uint32_t *errors)
{
    if (attempts) (*attempts)++;
    if (result == SMOOTHAF_D1_PUBLISH_OK) {
        if (accepted) (*accepted)++;
        series->published++;
        return;
    }
    series->integrity = 0;
    if (result == SMOOTHAF_D1_PUBLISH_DROP) {
        if (drops) (*drops)++;
        series->dropped_count++;
    } else if (result == SMOOTHAF_D1_PUBLISH_ERROR) {
        if (errors) (*errors)++;
        series->error_count++;
    } else if (result == SMOOTHAF_D1_PUBLISH_PARTIAL) {
        series->partial_count++;
    }
}

static inline int smoothaf_d1_series_start(
    struct smoothaf_d1_series *series, struct smoothaf_d1_gate *gate,
    uint32_t id, uint32_t started_ms, uint32_t requested,
    const struct smoothaf_d1_context *context,
    smoothaf_d1_publish_start_fn publish_start,
    smoothaf_d1_publish_context_fn publish_context_event,
    smoothaf_d1_publish_context_fn publish_context_aux_event,
    smoothaf_d1_publish_sample_fn publish_sample,
    smoothaf_d1_publish_end_fn publish_end, void *publish_context,
    int *reason)
{
    int result;
    if (!series || !gate || !id || requested == 0u ||
        requested > SMOOTHAF_D1_MAX_SAMPLES) {
        return smoothaf_d1_gate_reject(reason, SMOOTHAF_D1_REJECT_INVALID);
    }
    smoothaf_d1_series_zero(series);
    if (!smoothaf_d1_gate_acquire(gate, reason)) return 0;
    series->gate = gate;
    series->owner = SMOOTHAF_D1_OWNER_D1;
    series->generation = gate->state.generation;
    series->id = id;
    series->next_sequence = 0;
    series->started_ms = started_ms;
    series->requested = requested;
    series->status = SMOOTHAF_D1_STATUS_ACTIVE;
    series->integrity = 1;
    if (context) series->context = *context;
    series->publish_start = publish_start;
    series->publish_context_event = publish_context_event;
    series->publish_context_aux_event = publish_context_aux_event;
    series->publish_sample = publish_sample;
    series->publish_end = publish_end;
    series->publish_context = publish_context;
    if (publish_start) {
        result = publish_start(series, publish_context);
        smoothaf_d1_count_independent_publish(series, result,
            &series->start_attempts, &series->start_accepted,
            &series->start_drops, &series->start_errors);
        if (result == SMOOTHAF_D1_PUBLISH_ERROR ||
            result == SMOOTHAF_D1_PUBLISH_PARTIAL)
        {
            series->status = SMOOTHAF_D1_STATUS_ERROR;
            smoothaf_d1_gate_end(series->gate, series->owner);
        }
    }
    if (series->status == SMOOTHAF_D1_STATUS_ACTIVE && publish_context_event) {
        result = publish_context_event(series, publish_context);
        smoothaf_d1_count_independent_publish(series, result,
            &series->context_attempts, &series->context_accepted,
            &series->context_drops, &series->context_errors);
        if (result == SMOOTHAF_D1_PUBLISH_ERROR ||
            result == SMOOTHAF_D1_PUBLISH_PARTIAL)
        {
            series->status = SMOOTHAF_D1_STATUS_ERROR;
            smoothaf_d1_gate_end(series->gate, series->owner);
        }
    }
    if (series->status == SMOOTHAF_D1_STATUS_ACTIVE && publish_context_aux_event) {
        result = publish_context_aux_event(series, publish_context);
        smoothaf_d1_count_independent_publish(series, result,
            &series->context_aux_attempts, &series->context_aux_accepted,
            &series->context_aux_drops, &series->context_aux_errors);
        if (result == SMOOTHAF_D1_PUBLISH_ERROR ||
            result == SMOOTHAF_D1_PUBLISH_PARTIAL)
        {
            series->status = SMOOTHAF_D1_STATUS_ERROR;
            smoothaf_d1_gate_end(series->gate, series->owner);
        }
    }
    if (reason) *reason = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline int smoothaf_d1_series_record_metric(struct smoothaf_d1_series *series,
                                                   uint32_t sequence,
                                                   uint32_t timestamp_ms,
                                                   int32_t score,
                                                   int32_t mean,
                                                   int32_t sad,
                                                   int32_t laplacian,
                                                   uint32_t valid,
                                                   uint32_t confident,
                                                   uint32_t producer_id)
{
    struct smoothaf_d1_sample *sample;
    int result;
    if (!series || series->status != SMOOTHAF_D1_STATUS_ACTIVE) {
        if (series) series->last_reject = SMOOTHAF_D1_REJECT_NOT_ACTIVE;
        return 0;
    }
    if (smoothaf_d1_gate_cancelled(series->gate)) {
        series->last_reject = SMOOTHAF_D1_REJECT_STOP_PANIC;
        series->status = SMOOTHAF_D1_STATUS_CANCELLED;
        series->ended_ms = timestamp_ms;
        smoothaf_d1_gate_end(series->gate, series->owner);
        return 0;
    }
    if (!smoothaf_d1_gate_owns_generation(series->gate, series->generation)) {
        series->foreign_producer_count++;
        series->integrity = 0;
        series->last_reject = SMOOTHAF_D1_REJECT_FOREIGN_PRODUCER;
        return 0;
    }
    if (producer_id != series->owner) {
        series->foreign_producer_count++;
        series->integrity = 0;
        series->last_reject = SMOOTHAF_D1_REJECT_FOREIGN_PRODUCER;
        return 0;
    }
    if (series->produced >= series->requested ||
        series->produced >= SMOOTHAF_D1_MAX_SAMPLES) {
        series->last_reject = SMOOTHAF_D1_REJECT_LIMIT;
        return 0;
    }
    if (sequence > series->next_sequence) {
        series->sequence_gaps += sequence - series->next_sequence;
        series->dropped_count += sequence - series->next_sequence;
        series->integrity = 0;
    } else if (sequence < series->next_sequence) {
        series->error_count++;
        series->integrity = 0;
        series->last_reject = SMOOTHAF_D1_REJECT_INVALID;
        return 0;
    }
    sample = &series->samples[series->produced++];
    sample->sequence = sequence;
    sample->timestamp_ms = timestamp_ms;
    sample->score = score;
    sample->mean = mean;
    sample->sad = sad;
    sample->laplacian = laplacian;
    sample->valid = valid ? 1u : 0u;
    sample->confident = confident ? 1u : 0u;
    sample->producer_id = producer_id;
    series->next_sequence = sequence + 1u;
    if (series->publish_sample) {
        result = series->publish_sample(sample, series->publish_context);
        smoothaf_d1_count_independent_publish(series, result,
            &series->sample_attempts, &series->sample_accepted,
            &series->sample_drops, &series->sample_errors);
        if (result == SMOOTHAF_D1_PUBLISH_ERROR ||
            result == SMOOTHAF_D1_PUBLISH_PARTIAL)
        {
            series->status = SMOOTHAF_D1_STATUS_ERROR;
            smoothaf_d1_gate_end(series->gate, series->owner);
        }
    }
    series->last_reject = SMOOTHAF_D1_REJECT_NONE;
    return 1;
}

static inline int smoothaf_d1_series_record(struct smoothaf_d1_series *series,
                                             uint32_t sequence,
                                             uint32_t timestamp_ms,
                                             int32_t score,
                                             uint32_t producer_id)
{
    return smoothaf_d1_series_record_metric(series, sequence, timestamp_ms, score,
                                            0, 0, 0, score >= 0, 0, producer_id);
}

static inline int smoothaf_d1_series_finish(struct smoothaf_d1_series *series,
                                            int status, uint32_t ended_ms)
{
    int result;
    if (!series || series->status == SMOOTHAF_D1_STATUS_IDLE) return 0;
    if (series->end_published) {
        smoothaf_d1_gate_end(series->gate, series->owner);
        return 0;
    }
    if (series->status == SMOOTHAF_D1_STATUS_ACTIVE)
        series->status = status;
    if (series->status == SMOOTHAF_D1_STATUS_NORMAL &&
        (series->produced != series->requested || !series->integrity ||
         series->start_accepted != series->start_attempts ||
         series->context_accepted != series->context_attempts ||
         series->context_aux_accepted != series->context_aux_attempts ||
         series->sample_accepted != series->sample_attempts ||
         series->end_accepted != series->end_attempts))
        series->status = SMOOTHAF_D1_STATUS_ERROR;
    series->ended_ms = ended_ms;
    if (series->publish_end) {
        result = series->publish_end(series, series->publish_context);
        smoothaf_d1_count_independent_publish(series, result,
            &series->end_attempts, &series->end_accepted,
            &series->end_drops, &series->end_errors);
        series->end_published = 1;
        if (result != SMOOTHAF_D1_PUBLISH_OK)
            series->status = SMOOTHAF_D1_STATUS_ERROR;
    }
    smoothaf_d1_gate_end(series->gate, series->owner);
    return 1;
}

static inline int smoothaf_d1_series_cancel(struct smoothaf_d1_series *series,
                                            uint32_t ended_ms)
{
    return smoothaf_d1_series_finish(series, SMOOTHAF_D1_STATUS_CANCELLED,
                                     ended_ms);
}

#endif
