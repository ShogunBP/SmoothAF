#include <stdio.h>
#include "../src/smoothaf_diagnostics.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    struct smoothaf_log_buffer log;
    struct smoothaf_log_entry out[SMOOTHAF_DIAG_CAPACITY];
    size_t n;
    size_t i;

    smoothaf_log_init(&log);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == 0 && log.count == 0 && log.dropped == 0, "empty buffer");

    smoothaf_log_write(&log, SMOOTHAF_LOG_MOVE, 1, 2, 3);
    CHECK(log.count == 0, "logging starts disabled");
    smoothaf_log_set_enabled(&log, 1);
    smoothaf_log_tick(&log, 10);
    smoothaf_log_write_extended(&log, SMOOTHAF_LOG_MEASURE, 10, 20, 30, 40, 50);
    smoothaf_log_tick(&log, 5);
    smoothaf_log_write(&log, SMOOTHAF_LOG_TASK_END, 6, 7, 8);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == 2 && out[0].type == SMOOTHAF_LOG_MEASURE &&
          out[0].timestamp_ms == 10 && out[0].value5 == 50 &&
          out[1].type == SMOOTHAF_LOG_TASK_END && out[1].timestamp_ms == 15,
          "chronological entries and timestamps");

    for (i = 0; i < SMOOTHAF_DIAG_CAPACITY + 3; i++)
        smoothaf_log_write(&log, SMOOTHAF_LOG_MOVE, (int32_t)i, 0, 0);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == SMOOTHAF_DIAG_CAPACITY && log.count == SMOOTHAF_DIAG_CAPACITY &&
          log.dropped == 5 && out[0].value1 == 3 &&
          out[SMOOTHAF_DIAG_CAPACITY - 1].value1 == SMOOTHAF_DIAG_CAPACITY + 2,
          "wrap-around and dropped count");

    smoothaf_log_clear(&log);
    CHECK(log.count == 0 && log.head == 0 && log.dropped == 0,
          "clear history");

    if (failures) {
        printf("%d diagnostics test(s) FAILED\n", failures);
        return 1;
    }
    printf("SmoothAF diagnostics RAM tests passed\n");
    return 0;
}
