#include <stdio.h>
#include "../src/smoothaf_diagnostics.h"

static int failures;
#define CHECK(c,m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    struct smoothaf_log_buffer log;
    struct smoothaf_log_entry out[SMOOTHAF_DIAG_CAPACITY];
    size_t i, n;
    smoothaf_log_init(&log);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == 0, "empty buffer");
    smoothaf_log_write(&log, SMOOTHAF_LOG_MOVE, 1, 2, 3);
    CHECK(log.count == 0, "disabled logging");
    smoothaf_log_set_enabled(&log, 1);
    smoothaf_log_tick(&log, 10);
    smoothaf_log_write_extended(&log, SMOOTHAF_LOG_MEASURE, 1, 2, 3, 4, 5);
    CHECK(log.count == 1 && log.entries[0].timestamp_ms == 10, "timestamp and payload");
    for (i = 0; i < SMOOTHAF_DIAG_CAPACITY + 3; i++)
        smoothaf_log_write(&log, SMOOTHAF_LOG_MONITOR, (int)i, 0, 0);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == SMOOTHAF_DIAG_CAPACITY, "snapshot limit");
    CHECK(log.dropped == 4, "dropped count");
    CHECK(out[0].value1 == 3 && out[n - 1].value1 == 34, "chronological wrap");
    smoothaf_log_clear(&log);
    CHECK(log.count == 0 && log.dropped == 0, "clear history");
    if (failures) return 1;
    puts("SmoothAF diagnostics RAM tests passed");
    return 0;
}
