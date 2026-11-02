#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_log_export.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

struct sink { char data[8192]; size_t used; int fail; int short_write; };
static int sink_write(void *context, const char *data, size_t length)
{
    struct sink *sink = context;
    size_t n = sink->short_write && length ? length - 1 : length;
    if (sink->fail || sink->used + n > sizeof(sink->data)) return -1;
    memcpy(sink->data + sink->used, data, n);
    sink->used += n;
    return (int)n;
}

int main(void)
{
    struct smoothaf_log_entry entries[2];
    struct smoothaf_af_log_buffer af_log;
    struct smoothaf_log_entry af_snapshot[SMOOTHAF_AF_DIAG_CAPACITY];
    struct smoothaf_log_export_metadata metadata = { "ACTIVE", 500, 498, 10, 99, 3 };
    struct smoothaf_log_export_result result;
    struct sink sink = {{0}, 0, 0, 0};
    int rc;

    smoothaf_af_log_init(&af_log);
    for (int i = 0; i < SMOOTHAF_AF_DIAG_CAPACITY + 3; i++)
        smoothaf_af_log_write(&af_log, (uint32_t)i, SMOOTHAF_LOG_CONTROLLER,
                              i, 0, 0, 0, 0);
    CHECK(af_log.count == SMOOTHAF_AF_DIAG_CAPACITY && af_log.dropped == 3,
          "AF history retains 128 entries and counts drops");
    CHECK(smoothaf_af_log_snapshot(&af_log, af_snapshot,
                                   SMOOTHAF_AF_DIAG_CAPACITY) ==
          SMOOTHAF_AF_DIAG_CAPACITY && af_snapshot[0].value1 == 3,
          "AF history snapshot is chronological");

    memset(entries, 0, sizeof(entries));
    entries[0].type = SMOOTHAF_LOG_MOVE;
    entries[0].timestamp_ms = 20;
    entries[0].value1 = 1;
    entries[1].type = SMOOTHAF_LOG_TASK_END;
    entries[1].timestamp_ms = 40;
    entries[1].value1 = 7;
    rc = smoothaf_log_export_snapshot(entries, 2, &metadata, sink_write, &sink, &result);
    CHECK(rc == 0 && result.events == 2, "serializes all events");
    CHECK(strstr(sink.data, "Mode: ACTIVE") != 0, "serializes mode");
    CHECK(strstr(sink.data, "Events Dropped: 3") != 0, "serializes dropped count");
    CHECK(strstr(sink.data, "20 MOVE") < strstr(sink.data, "40 TASK-END"), "preserves chronology");

    sink.used = 0;
    sink.short_write = 1;
    rc = smoothaf_log_export_snapshot(entries, 2, &metadata, sink_write, &sink, &result);
    CHECK(rc != 0, "rejects short write");

    sink.used = 0;
    sink.short_write = 0;
    sink.fail = 1;
    rc = smoothaf_log_export_snapshot(entries, 2, &metadata, sink_write, &sink, &result);
    CHECK(rc != 0, "propagates backend failure");

    if (failures) {
        printf("%d log export test(s) FAILED\n", failures);
        return 1;
    }
    printf("SmoothAF log export tests passed\n");
    return 0;
}
