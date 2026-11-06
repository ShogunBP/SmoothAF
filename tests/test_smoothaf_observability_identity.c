#include <stdio.h>
#include "../src/smoothaf_observability.h"

static int failures;
static int published;

static bool publish(void *context, const struct smoothaf_observability_event *event)
{
    (void)context;
    (void)event;
    published++;
    return true;
}

static void event(struct smoothaf_observability_event *value,
                  uint16_t episode, uint16_t logical,
                  uint8_t namespace, uint32_t generation,
                  enum smoothaf_observability_phase phase)
{
    struct smoothaf_observability_snapshot snapshot;
    struct smoothaf_observability_result result;
    smoothaf_observability_snapshot_init(&snapshot);
    snapshot.timestamp_ms = phase ? 101u : 100u;
    snapshot.available_mask = SMOOTHAF_OBS_CORE_FIELDS;
    snapshot.yuv_buffer_address = 0x1000u;
    snapshot.width = 720;
    snapshot.pitch = 1440;
    snapshot.height = 480;
    snapshot.liveview = 1u;
    snapshot.movie_mode = 1u;
    snapshot.recording_class = 1u;
    smoothaf_observability_classify(&snapshot, &snapshot,
                                    SMOOTHAF_OBS_PROFILE_CORE, &result);
    smoothaf_observability_make_event(value, &snapshot, &result, episode, 0u,
                                      logical, phase);
    smoothaf_observability_event_set_identity(value, namespace, generation);
}

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("FAIL: %s\n", message); failures++; } } while (0)

int main(void)
{
    struct smoothaf_observability_controller controller;
    struct smoothaf_observability_event before;
    struct smoothaf_observability_event after;
    smoothaf_observability_controller_init(&controller, 0u);
    smoothaf_observability_controller_set_identity(
        &controller, SMOOTHAF_OBS_IDENTITY_AF, 10u);
    event(&before, 7u, 1u, SMOOTHAF_OBS_IDENTITY_AF, 10u,
          SMOOTHAF_OBS_PHASE_BEFORE);
    event(&after, 7u, 1u, SMOOTHAF_OBS_IDENTITY_AF, 10u,
          SMOOTHAF_OBS_PHASE_AFTER);
    CHECK(smoothaf_observability_submit_pair(&controller, &before, &after,
                                             publish, 0),
          "same identity pairs");
    event(&before, 7u, 1u, SMOOTHAF_OBS_IDENTITY_D1, 11u,
          SMOOTHAF_OBS_PHASE_BEFORE);
    event(&after, 7u, 1u, SMOOTHAF_OBS_IDENTITY_D1, 11u,
          SMOOTHAF_OBS_PHASE_AFTER);
    CHECK(!smoothaf_observability_submit_pair(&controller, &before, &after,
                                              publish, 0),
          "foreign namespace is rejected");
    CHECK(published == 2, "only the complete AF pair was published");
    event(&before, 7u, 1u, SMOOTHAF_OBS_IDENTITY_AF, 12u,
          SMOOTHAF_OBS_PHASE_BEFORE);
    event(&after, 7u, 1u, SMOOTHAF_OBS_IDENTITY_AF, 12u,
          SMOOTHAF_OBS_PHASE_AFTER);
    CHECK(!smoothaf_observability_submit_pair(&controller, &before, &after,
                                              publish, 0),
          "stale generation is rejected");
    if (failures) return 1;
    printf("SmoothAF observability identity tests passed\n");
    return 0;
}
