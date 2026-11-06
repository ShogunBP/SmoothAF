#include <stdio.h>
#include <stdint.h>
#include "../src/smoothaf_observability.h"

static void fill_snapshot(struct smoothaf_observability_snapshot *snapshot)
{
    smoothaf_observability_snapshot_init(snapshot);
    snapshot->timestamp_ms = 1000u;
    snapshot->available_mask = SMOOTHAF_OBS_CORE_FIELDS;
    snapshot->yuv_buffer_address = 0x1000u;
    snapshot->width = 720;
    snapshot->pitch = 1440;
    snapshot->height = 480;
    snapshot->liveview = 1u;
    snapshot->movie_mode = 1u;
    snapshot->recording_class = 1u;
}

int main(void)
{
    struct smoothaf_observability_snapshot before;
    struct smoothaf_observability_snapshot after;
    struct smoothaf_observability_result result;
    struct smoothaf_observability_event event;
    uint8_t bytes[SMOOTHAF_OBS_RECORD_SIZE];
    size_t i;

    fill_snapshot(&before);
    fill_snapshot(&after);
    if (!smoothaf_observability_classify(&before, &after,
                                         SMOOTHAF_OBS_PROFILE_CORE, &result) ||
        !smoothaf_observability_make_event(&event, &before, &result,
                                           3u, 7u, 1u,
                                           SMOOTHAF_OBS_PHASE_BEFORE) ||
        !smoothaf_observability_serialize(&event, bytes))
        return 1;
    for (i = 0; i < sizeof(bytes); ++i)
        printf("%02x", (unsigned)bytes[i]);
    putchar('\n');
    return 0;
}
