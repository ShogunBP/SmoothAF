#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_diagnostics.h"

static int failures;
#define CHECK(c,m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    struct smoothaf_log_buffer log;
    struct smoothaf_log_entry out[SMOOTHAF_DIAG_CAPACITY];
    size_t seen = 0;
    size_t page;
    size_t i, n;
    smoothaf_log_init(&log);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == 0 && smoothaf_log_total(&log) == 0, "empty buffer");
    smoothaf_log_write(&log, SMOOTHAF_LOG_MOVE, 1, 2, 3);
    CHECK(log.count == 0, "disabled logging");
    smoothaf_log_set_enabled(&log, 1);
    smoothaf_log_tick(&log, 10);
    smoothaf_log_write_extended(&log, SMOOTHAF_LOG_MEASURE, 1, 2, 3, 4, 5);
    smoothaf_log_tick(&log, 5);
    smoothaf_log_write(&log, SMOOTHAF_LOG_TASK_END, 6, 7, 8);
    CHECK(log.count == 2 && log.entries[0].timestamp_ms == 10 &&
          log.entries[0].value5 == 5, "timestamp and payload");
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == 2 && out[0].type == SMOOTHAF_LOG_MEASURE &&
          out[1].type == SMOOTHAF_LOG_TASK_END &&
          out[1].timestamp_ms == 15,
          "chronological entries and timestamps");
    for (i = 0; i < SMOOTHAF_DIAG_CAPACITY + 3; i++)
        smoothaf_log_write(&log, SMOOTHAF_LOG_MONITOR, (int)i, 0, 0);
    n = smoothaf_log_snapshot(&log, out, SMOOTHAF_DIAG_CAPACITY);
    CHECK(n == SMOOTHAF_DIAG_CAPACITY, "snapshot limit");
    CHECK(log.dropped == 5, "dropped count");
    CHECK(smoothaf_log_total(&log) == SMOOTHAF_DIAG_CAPACITY + 5,
          "total is monotonic after the ring fills");
    CHECK(out[0].value1 == 3 && out[n - 1].value1 == SMOOTHAF_DIAG_CAPACITY + 2,
          "chronological wrap");

    page = smoothaf_diag_page_after_refresh(0, 0, 9, &seen);
    CHECK(page == 0 && seen == 0, "empty log has no page");
    page = smoothaf_diag_page_after_refresh(5, 5, 0, &seen);
    CHECK(page == 4 && seen == 5, "new records select the newest page");
    seen = 32;
    page = smoothaf_diag_page_after_refresh(SMOOTHAF_DIAG_CAPACITY,
                                            SMOOTHAF_DIAG_CAPACITY + 8,
                                            4, &seen);
    CHECK(page == SMOOTHAF_DIAG_CAPACITY - 1 && seen == SMOOTHAF_DIAG_CAPACITY + 8,
          "full buffer still follows new records");
    page = smoothaf_diag_page_after_refresh(SMOOTHAF_DIAG_CAPACITY,
                                            SMOOTHAF_DIAG_CAPACITY + 8,
                                            10, &seen);
    CHECK(page == 10, "manual page is preserved when no new record arrives");
    page = smoothaf_diag_page_after_refresh(3, SMOOTHAF_DIAG_CAPACITY + 8,
                                            10, &seen);
    CHECK(page == 2, "stale page is clamped");
    CHECK(strcmp(smoothaf_log_type_name(SMOOTHAF_LOG_MEASURE), "MEASURE") == 0 &&
          strcmp(smoothaf_log_type_name(SMOOTHAF_LOG_TASK_END), "TASK-END") == 0,
          "diagnostic record names");

    smoothaf_log_clear(&log);
    CHECK(log.count == 0 && log.dropped == 0 && smoothaf_log_total(&log) == 0,
          "clear history");
    if (failures) return 1;
    puts("SmoothAF diagnostics RAM tests passed");
    return 0;
}
