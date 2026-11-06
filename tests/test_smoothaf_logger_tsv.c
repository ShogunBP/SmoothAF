#include <stdint.h>
#include <stdio.h>

#include "../src/smoothaf_logger.h"

static int emit(struct smoothaf_persist_log_buffer *buf,
                uint32_t timestamp, uint16_t event_type,
                uint16_t episode, const int32_t payload[6])
{
    return smoothaf_persist_log_record(buf, timestamp, event_type, episode,
                                       payload) ? 0 : 1;
}

int main(void)
{
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    static const int32_t signed_payload[6] = {-2147483648, -1, 0, 1, 2147483647, -7};
    static const int32_t unsigned_payload[6] = {-1, -2, -3, -4, -5, -6};
    static const int32_t mixed_sample[6] = {-1, -2, 123, -456, 0, 1};
    uint32_t count;

    smoothaf_persist_logger_init(&buf);
    smoothaf_persist_logger_set_enabled(&buf, true);

    if (emit(&buf, 0u, SMOOTHAF_PERSIST_LOG_MEASUREMENT, 7u,
             signed_payload) ||
        emit(&buf, UINT32_MAX, SMOOTHAF_PERSIST_LOG_D1_SERIES_START, 8u,
             unsigned_payload) ||
        emit(&buf, 1u, SMOOTHAF_PERSIST_LOG_D1_SAMPLE, 8u,
             mixed_sample) ||
        emit(&buf, 2u, SMOOTHAF_PERSIST_LOG_D1_SERIES_END, 8u,
             unsigned_payload) ||
        emit(&buf, 3u, SMOOTHAF_PERSIST_LOG_D1_CONTEXT, 8u,
             unsigned_payload) ||
        emit(&buf, 4u, SMOOTHAF_PERSIST_LOG_D1_CONTEXT_AUX, 8u,
             unsigned_payload) ||
        emit(&buf, 5u, SMOOTHAF_PERSIST_LOG_MOVIE_GATE_REJECT, 0u,
             unsigned_payload) ||
        emit(&buf, 6u, SMOOTHAF_PERSIST_LOG_OBS_SNAPSHOT_V1, 9u,
             (const int32_t[]){0x00010301, 0x00001000, -1, -1, -1, -1}) ||
        emit(&buf, 7u, 99u, 10u, signed_payload))
        return 1;

    count = smoothaf_persist_log_consume_batch(&buf, &batch);
    if (count != 9u || batch.count != 9u)
        return 2;
    for (uint32_t i = 0; i < batch.count; ++i)
        puts(batch.lines[i]);
    return 0;
}
