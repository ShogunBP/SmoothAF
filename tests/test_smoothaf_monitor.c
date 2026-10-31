#include <stdio.h>
#include "../src/smoothaf_monitor.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static struct smoothaf_monitor_sample sample(int score, int mean, int valid, int confident)
{
    struct smoothaf_monitor_sample s;
    s.score = score;
    s.sad = score;
    s.laplacian = score + 1;
    s.mean = mean;
    s.valid = valid;
    s.confident = confident;
    s.metric = SMOOTHAF_MONITOR_METRIC_SAD;
    s.duration_ms = 12;
    return s;
}

int main(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    int from;
    int to;

    CHECK(smoothaf_monitor_interval_ms(SMOOTHAF_MONITOR_INTERVAL_1000) == 1000,
          "1000 ms interval");
    CHECK(smoothaf_monitor_interval_ms(SMOOTHAF_MONITOR_INTERVAL_DEFAULT) == 500,
          "500 ms default interval");
    CHECK(smoothaf_monitor_normalize_interval(99) == SMOOTHAF_MONITOR_INTERVAL_DEFAULT,
          "invalid interval uses default");
    CHECK(smoothaf_monitor_due(0, UINT32_MAX), "deadline wrap-around");

    smoothaf_monitor_init(&m, SMOOTHAF_MONITOR_METRIC_SAD,
                          SMOOTHAF_MONITOR_INTERVAL_DEFAULT);
    smoothaf_monitor_start(&m, SMOOTHAF_MONITOR_METRIC_SAD, 100);
    CHECK(m.enabled && m.metrics.next_due_ms == 100, "start schedules immediate sample");
    CHECK(smoothaf_monitor_take_transition(&m, &from, &to) &&
          to == SMOOTHAF_MONITOR_MONITORING, "start transition is visible");

    s = sample(1000, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 110);
    CHECK(m.have_reference && m.reference == 1000, "first sample initializes reference");
    CHECK(m.metrics.valid == 1, "valid sample counted");
    CHECK(m.metrics.duration_last == 12 && m.metrics.duration_max == 12,
          "sample duration tracked");
    smoothaf_monitor_record_start(&m, 120);
    smoothaf_monitor_record_start(&m, 620);
    CHECK(m.metrics.have_interval && m.metrics.actual_interval_ms == 500,
          "actual interval uses start-to-start");
    { struct smoothaf_monitor_snapshot snapshot;
      smoothaf_monitor_snapshot(&m, &snapshot);
      CHECK(snapshot.attempts == m.metrics.attempts &&
            snapshot.duration_average == 12,
            "performance snapshot is consistent"); }

    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 600);
    smoothaf_monitor_accept(&m, &s, 1100);
    smoothaf_monitor_accept(&m, &s, 1600);
    CHECK(m.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR,
          "persistent loss enters possible blur");
    CHECK(m.reference == 1000, "blur does not lower reference");

    s = sample(1000, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 2100);
    smoothaf_monitor_accept(&m, &s, 2600);
    smoothaf_monitor_accept(&m, &s, 3100);
    smoothaf_monitor_accept(&m, &s, 3600);
    smoothaf_monitor_accept(&m, &s, 4100);
    smoothaf_monitor_accept(&m, &s, 4600);
    smoothaf_monitor_accept(&m, &s, 5100);
    smoothaf_monitor_accept(&m, &s, 5600);
    smoothaf_monitor_accept(&m, &s, 6100);
    smoothaf_monitor_accept(&m, &s, 6600);
    smoothaf_monitor_accept(&m, &s, 7100);
    smoothaf_monitor_accept(&m, &s, 7600);
    CHECK(m.state == SMOOTHAF_MONITOR_STABLE, "persistent recovery is stable");

    s = sample(1000, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 2600);
    CHECK(m.state == SMOOTHAF_MONITOR_UNCERTAIN,
          "lighting change suspends decisions");

    s = sample(-1, 0, 0, 0);
    smoothaf_monitor_accept(&m, &s, 3100);
    CHECK(m.metrics.invalid == 1, "invalid sample counted separately");
    CHECK(m.reference == 1000, "invalid sample does not alter reference");

    smoothaf_monitor_stop(&m);
    CHECK(!m.enabled && m.state == SMOOTHAF_MONITOR_IDLE, "stop freezes session");

    if (failures) {
        printf("%d monitor test(s) FAILED\n", failures);
        return 1;
    }
    printf("SmoothAF monitor tests passed\n");
    return 0;
}
