#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_observability.h"

static int failures;
static int publish_count;
static int publish_limit = 100;
static struct smoothaf_observability_event published[8];

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("FAIL: %s\n", message); failures++; } } while (0)

static void fill_snapshot(struct smoothaf_observability_snapshot *snapshot,
                          uint32_t timestamp)
{
    smoothaf_observability_snapshot_init(snapshot);
    snapshot->timestamp_ms = timestamp;
    snapshot->available_mask = SMOOTHAF_OBS_CORE_FIELDS;
    snapshot->yuv_buffer_address = 0x1000u;
    snapshot->width = 720;
    snapshot->pitch = 1440;
    snapshot->height = 480;
    snapshot->liveview = 1;
    snapshot->movie_mode = 1;
    snapshot->recording_class = 1;
}

static bool capture_publish(void *context,
                            const struct smoothaf_observability_event *event)
{
    (void)context;
    if (publish_count >= publish_limit) return false;
    if (publish_count < (int)(sizeof(published) / sizeof(published[0])))
        published[publish_count] = *event;
    publish_count++;
    return true;
}

static void test_classification_and_serialization(void)
{
    struct smoothaf_observability_snapshot before;
    struct smoothaf_observability_snapshot after;
    struct smoothaf_observability_result result;
    struct smoothaf_observability_event event;
    uint8_t bytes[SMOOTHAF_OBS_RECORD_SIZE];

    fill_snapshot(&before, 1000u);
    fill_snapshot(&after, 1002u);
    CHECK(smoothaf_observability_classify(&before, &after,
                                          SMOOTHAF_OBS_PROFILE_CORE,
                                          &result), "CORE classification succeeds");
    CHECK(result.status == SMOOTHAF_OBS_STABLE_METADATA,
          "identical metadata is stable");
    CHECK(result.changed_mask == 0u && result.missing_mask == 0u,
          "stable masks are empty");
    CHECK(smoothaf_observability_make_event(&event, &before, &result, 3u, 7u,
                                            1u, SMOOTHAF_OBS_PHASE_BEFORE),
          "event construction succeeds");
    CHECK(smoothaf_observability_serialize(&event, bytes),
          "event serialization succeeds");
    CHECK(bytes[0] == 0xE8u && bytes[1] == 0x03u &&
          bytes[4] == 24u && bytes[5] == 0u &&
          bytes[6] == 3u && bytes[7] == 0u,
          "serialized header is little-endian and event 24");
    {
        static const uint8_t expected[SMOOTHAF_OBS_RECORD_SIZE] = {
            0xE8u, 0x03u, 0x00u, 0x00u, 0x18u, 0x00u, 0x03u, 0x00u,
            0x07u, 0x00u, 0x00u, 0x00u, 0x01u, 0x03u, 0x01u, 0x00u,
            0x00u, 0x10u, 0x00u, 0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
            0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
            0xFFu, 0xFFu, 0xFFu, 0xFFu
        };
        CHECK(memcmp(bytes, expected, sizeof(expected)) == 0,
              "C bytes match the Python OBS_SNAPSHOT_V1 fixture");
        CHECK(bytes[0] == 0xE8u && bytes[1] == 0x03u &&
              bytes[4] == 24u && bytes[5] == 0u &&
              bytes[6] == 3u && bytes[7] == 0u,
              "serialized header is little-endian and event 24");
    }
}

static void test_equivalence_off_and_transport(void)
{
    struct smoothaf_observability_snapshot before;
    struct smoothaf_observability_snapshot after;
    struct smoothaf_observability_result result;
    struct smoothaf_observability_event before_event;
    struct smoothaf_observability_event after_event;
    struct smoothaf_observability_controller controller;

    fill_snapshot(&before, 1000u);
    fill_snapshot(&after, 1001u);
    smoothaf_observability_classify(&before, &after,
                                   SMOOTHAF_OBS_PROFILE_CORE, &result);
    smoothaf_observability_make_event(&before_event, &before, &result, 3u, 7u,
                                      1u, SMOOTHAF_OBS_PHASE_BEFORE);
    smoothaf_observability_make_event(&after_event, &after, &result, 3u, 8u,
                                      1u, SMOOTHAF_OBS_PHASE_AFTER);
    smoothaf_observability_controller_init(&controller, 100u);
    publish_count = 0;
    publish_limit = 100;
    smoothaf_observability_controller_set_identity(
        &controller, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    smoothaf_observability_event_set_identity(
        &before_event, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    smoothaf_observability_event_set_identity(
        &after_event, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    CHECK(smoothaf_observability_submit_pair(&controller, &before_event,
                                             &after_event, capture_publish, 0),
          "first complete pair publishes");
    CHECK(publish_count == 2 && controller.dropped_events == 0,
          "complete pair publishes two events");

    publish_limit = 3;
    fill_snapshot(&after, 1102u);
    smoothaf_observability_event_set_identity(
        &before_event, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    smoothaf_observability_event_set_identity(
        &after_event, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    smoothaf_observability_classify(&before, &after,
                                   SMOOTHAF_OBS_PROFILE_CORE, &result);
    smoothaf_observability_make_event(&after_event, &after, &result, 3u, 9u,
                                      1u, SMOOTHAF_OBS_PHASE_AFTER);
    smoothaf_observability_event_set_identity(
        &after_event, SMOOTHAF_OBS_IDENTITY_AF, 1u);
    CHECK(!smoothaf_observability_submit_pair(&controller, &before_event,
                                              &after_event, capture_publish, 0),
          "partial publisher failure is reported");
    CHECK(controller.incomplete_pairs == 1u && controller.dropped_events == 1u,
          "partial pair is counted as incomplete and dropped");
    CHECK(controller.has_pending,
          "failed publication remains pending for a later best-effort flush");
    publish_limit = 100;
    CHECK(smoothaf_observability_flush_pending(&controller, capture_publish, 0),
          "pending pair can be retried after logger recovery");
}

static void test_wrap_and_invalid_data(void)
{
    struct smoothaf_observability_controller controller;
    struct smoothaf_observability_snapshot before;
    struct smoothaf_observability_snapshot after;
    struct smoothaf_observability_result result;
    struct smoothaf_observability_event event;
    uint8_t bytes[SMOOTHAF_OBS_RECORD_SIZE];

    smoothaf_observability_controller_init(&controller, 0u);
    controller.next_logical_id = 0xFFFEu;
    CHECK(smoothaf_observability_next_logical_id(&controller) == 0xFFFEu,
          "last valid logical id is returned");
    CHECK(smoothaf_observability_next_logical_id(&controller) == 1u,
          "logical id wraps over reserved value");
    fill_snapshot(&before, 0u);
    fill_snapshot(&after, 0xFFFFFFFFu);
    smoothaf_observability_classify(&before, &after,
                                   SMOOTHAF_OBS_PROFILE_CORE, &result);
    CHECK(result.timestamp_ambiguous, "ambiguous timestamp is reported");
    smoothaf_observability_make_event(&event, &before, &result, 3u, 1u, 1u,
                                      SMOOTHAF_OBS_PHASE_BEFORE);
    event.flags |= SMOOTHAF_OBS_FLAG_MVR_WRAPPED;
    CHECK(!smoothaf_observability_serialize(&event, bytes),
          "invalid MVR wrap flag is rejected");
}

int main(void)
{
    CHECK(SMOOTHAF_OBSERVABILITY_ENABLED == 0,
          "observability integration is OFF by default");
    test_classification_and_serialization();
    test_equivalence_off_and_transport();
    test_wrap_and_invalid_data();
    if (failures) {
        printf("SmoothAF observability tests failed: %d\n", failures);
        return 1;
    }
    printf("SmoothAF observability tests passed\n");
    return 0;
}
