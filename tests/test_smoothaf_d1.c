#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_d1.h"

static int failures;
static int lock_held;
static int lock_calls;
static int unlock_calls;
static int start_calls;
static int context_calls;
static int context_aux_calls;
static int sample_calls;
static int end_calls;
static int publish_result;

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("FAIL: %s\n", message); failures++; } } while (0)

static int mock_lock(void *context)
{
    (void)context;
    lock_calls++;
    if (lock_held) return 0;
    lock_held = 1;
    return 1;
}

static void mock_unlock(void *context)
{
    (void)context;
    unlock_calls++;
    lock_held = 0;
}

static int publish_start(const void *value, void *context)
{
    (void)value; (void)context;
    start_calls++;
    return publish_result;
}

static int publish_context(const void *value, void *context)
{
    const struct smoothaf_d1_series *series = (const struct smoothaf_d1_series *)value;
    (void)context;
    CHECK(series->context.scene[0] != '\0', "scene context is published separately");
    context_calls++;
    return publish_result;
}

static int publish_context_aux(const void *value, void *context)
{
    const struct smoothaf_d1_series *series = (const struct smoothaf_d1_series *)value;
    (void)context;
    CHECK(series->context.build_id[0] != '\0', "build context is published separately");
    context_aux_calls++;
    return publish_result;
}

static int publish_sample(const void *value, void *context)
{
    const struct smoothaf_d1_sample *sample = (const struct smoothaf_d1_sample *)value;
    (void)context;
    CHECK(sample->producer_id == SMOOTHAF_D1_OWNER_D1, "published sample has D1 identity");
    sample_calls++;
    return publish_result;
}

static int publish_end(const void *value, void *context)
{
    const struct smoothaf_d1_series *series = (const struct smoothaf_d1_series *)value;
    (void)context;
    CHECK(series->produced <= SMOOTHAF_D1_MAX_SAMPLES, "published series is bounded");
    end_calls++;
    return publish_result;
}

static void reset_mocks(void)
{
    lock_held = 0;
    lock_calls = 0;
    unlock_calls = 0;
    start_calls = 0;
    context_calls = 0;
    context_aux_calls = 0;
    sample_calls = 0;
    end_calls = 0;
    publish_result = SMOOTHAF_D1_PUBLISH_OK;
}

static void test_exclusion_matrix(void)
{
    struct smoothaf_d1_gate gate;
    int reason = 0;
    smoothaf_d1_gate_init(&gate, mock_lock, mock_unlock, 0);

    CHECK(smoothaf_d1_gate_start_monitor(&gate, 0, &reason), "monitor can start when idle");
    CHECK(!smoothaf_d1_gate_acquire(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_MONITOR, "monitor blocks D1");
    smoothaf_d1_gate_end(&gate, SMOOTHAF_D1_OWNER_MONITOR);

    CHECK(smoothaf_d1_gate_acquire(&gate, &reason), "D1 acquires idle gate");
    CHECK(!smoothaf_d1_gate_start_monitor(&gate, 0, &reason) &&
          reason == SMOOTHAF_D1_REJECT_BUSY, "D1 blocks monitor");
    CHECK(!smoothaf_d1_gate_start_focus_test(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_BUSY, "D1 blocks Focus Test");
    smoothaf_d1_gate_end(&gate, SMOOTHAF_D1_OWNER_D1);

    CHECK(smoothaf_d1_gate_start_focus_test(&gate, &reason), "Focus Test can start when idle");
    CHECK(!smoothaf_d1_gate_acquire(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_FOCUS_TEST, "Focus Test blocks D1");
    smoothaf_d1_gate_end(&gate, SMOOTHAF_D1_OWNER_FOCUS_TEST);

    CHECK(smoothaf_d1_gate_start_measurement(&gate, 99, &reason),
          "ordinary measurement producer can start when idle");
    CHECK(!smoothaf_d1_gate_acquire(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_BUSY, "measurement producer blocks D1");
    smoothaf_d1_gate_end(&gate, SMOOTHAF_D1_OWNER_MEASUREMENT);

    smoothaf_d1_gate_signal_stop(&gate, 0);
    CHECK(!smoothaf_d1_gate_acquire(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_STOP_PANIC, "STOP blocks D1");
    smoothaf_d1_gate_clear_stop(&gate);
    smoothaf_d1_gate_signal_stop(&gate, 1);
    CHECK(!smoothaf_d1_gate_acquire(&gate, &reason) &&
          reason == SMOOTHAF_D1_REJECT_STOP_PANIC, "PANIC blocks D1");
    CHECK(lock_calls == unlock_calls, "gate lock is released on every path");
}

static void test_series_identity_and_limit(void)
{
    struct smoothaf_d1_gate gate;
    struct smoothaf_d1_series series;
    struct smoothaf_d1_context context;
    int reason = 0;
    unsigned int i;

    smoothaf_d1_gate_init(&gate, mock_lock, mock_unlock, 0);
    smoothaf_d1_context_clear(&context);
    smoothaf_d1_copy_text(context.scene, sizeof(context.scene), "SMTH001 reference scene");
    smoothaf_d1_copy_text(context.capture_mode, sizeof(context.capture_mode), "LiveView");
    smoothaf_d1_copy_text(context.build_id, sizeof(context.build_id), "test-build");
    CHECK(smoothaf_d1_series_start(&series, &gate, 17, 1000, 3, &context,
                                   publish_start, publish_context, publish_context_aux,
                                   publish_sample, publish_end, 0, &reason), "D1 series starts with bounded request");
    CHECK(series.id == 17 && series.started_ms == 1000 && series.requested == 3 &&
          series.owner == SMOOTHAF_D1_OWNER_D1,
          "series identity, owner and request are retained");
    CHECK(strcmp(series.context.scene, "SMTH001 reference scene") == 0,
          "capture context is copied into series");
    CHECK(context_calls == 1 && context_aux_calls == 1,
          "context halves are published as independent events");
    CHECK(smoothaf_d1_series_record(&series, 0, 1100, 10, SMOOTHAF_D1_OWNER_D1),
          "first D1 sample is accepted");
    CHECK(smoothaf_d1_series_record(&series, 2, 1300, 12, SMOOTHAF_D1_OWNER_D1),
          "sample with sequence gap is retained but invalidates integrity");
    CHECK(series.sequence_gaps == 1 && series.dropped_count == 1 && !series.integrity,
          "sequence gap is explicit loss, not silently accepted");
    CHECK(smoothaf_d1_series_record(&series, 3, 1400, 13, 99) == 0 &&
          series.foreign_producer_count == 1, "foreign producer is never attributed");
    CHECK(smoothaf_d1_series_finish(&series, SMOOTHAF_D1_STATUS_NORMAL, 1500),
          "series finishes and releases gate");
    CHECK(series.status == SMOOTHAF_D1_STATUS_ERROR && series.ended_ms == 1500,
          "incomplete or lossy series is not classified normal");
    CHECK(gate.state.owner == SMOOTHAF_D1_OWNER_NONE, "finish releases D1 exclusion");

    reset_mocks();
    smoothaf_d1_gate_init(&gate, mock_lock, mock_unlock, 0);
    CHECK(smoothaf_d1_series_start(&series, &gate, 18, 2000,
                                   SMOOTHAF_D1_MAX_SAMPLES, 0,
                                   0, 0, 0, 0, 0, 0, &reason),
          "maximum 30 sample series starts");
    for (i = 0; i < SMOOTHAF_D1_MAX_SAMPLES; ++i)
        CHECK(smoothaf_d1_series_record(&series, i, 2100 + i, (int32_t)i,
                                        SMOOTHAF_D1_OWNER_D1), "sample within limit accepted");
    CHECK(!smoothaf_d1_series_record(&series, SMOOTHAF_D1_MAX_SAMPLES,
                                     2200, 99, SMOOTHAF_D1_OWNER_D1),
          "31st sample rejected");
    CHECK(series.produced == SMOOTHAF_D1_MAX_SAMPLES, "series never exceeds 30 samples");
    CHECK(smoothaf_d1_series_finish(&series, SMOOTHAF_D1_STATUS_NORMAL, 2300),
          "full series finishes");
    CHECK(series.status == SMOOTHAF_D1_STATUS_NORMAL,
          "complete lossless series is normal");
}

static void test_cancel_and_publish_loss(void)
{
    struct smoothaf_d1_gate gate;
    struct smoothaf_d1_series series;
    int reason = 0;
    reset_mocks();
    smoothaf_d1_gate_init(&gate, mock_lock, mock_unlock, 0);
    publish_result = SMOOTHAF_D1_PUBLISH_DROP;
    CHECK(smoothaf_d1_series_start(&series, &gate, 19, 3000, 1, 0,
                                   publish_start, 0, 0, publish_sample, publish_end, 0,
                                   &reason), "series starts despite observable start drop");
    CHECK(!series.integrity && series.dropped_count == 1,
          "start drop invalidates integrity and is counted");
    publish_result = SMOOTHAF_D1_PUBLISH_OK;
    smoothaf_d1_gate_signal_stop(&gate, 0);
    CHECK(!smoothaf_d1_series_record(&series, 0, 3100, 22, SMOOTHAF_D1_OWNER_D1),
          "STOP cancels before another sample");
    CHECK(series.status == SMOOTHAF_D1_STATUS_CANCELLED &&
          gate.state.owner == SMOOTHAF_D1_OWNER_NONE,
          "cancel releases exclusion");
}

int main(void)
{
    reset_mocks();
    test_exclusion_matrix();
    test_series_identity_and_limit();
    test_cancel_and_publish_loss();
    if (failures) {
        printf("SmoothAF D1 tests failed: %d\n", failures);
        return 1;
    }
    printf("SmoothAF D1 tests passed\n");
    return 0;
}
