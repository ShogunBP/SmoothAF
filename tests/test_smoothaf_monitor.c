#include <stdio.h>
#include <string.h>
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
    struct smoothaf_vlog_af af;

    smoothaf_vlog_af_init(&af);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_OFF && !af.armed,
          "Vlog AF initializes OFF and disarmed");
    CHECK(smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1),
          "monitor task itself does not block manual arming");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_DRY_RUN, 0, 0, 0),
          "arming needs an enabled monitor");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 0),
          "another focus task blocks arming");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_OFF, 1, 0, 0),
          "OFF mode cannot be armed");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1000);
    CHECK(af.armed && af.state == SMOOTHAF_VLOG_AF_WAITING,
          "new episode is manually armed and waiting");
    CHECK(!smoothaf_vlog_af_should_probe(&af, SMOOTHAF_MONITOR_STABLE,
                                         SMOOTHAF_VLOG_AF_DRY_RUN),
          "stable monitor leaves the episode waiting");
    CHECK(smoothaf_vlog_af_should_probe(&af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                                        SMOOTHAF_VLOG_AF_DRY_RUN),
          "confirmed blur promotes the episode to probing");
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(smoothaf_vlog_af_dry_run_request(&af, 0, 100) &&
          af.commands == 1 && af.dry_run_requests == 1 &&
          af.state == SMOOTHAF_VLOG_AF_SETTLING,
          "DRY RUN records an intent without physical adapter");
    CHECK(af.awaiting_result && af.settle_until_ms == 220,
          "DRY RUN uses settle deadline");
    CHECK(!smoothaf_vlog_af_result_due(&af, 219) &&
          smoothaf_vlog_af_result_due(&af, 220),
          "results wait until settle deadline");
    smoothaf_vlog_af_record_result(&af, 1100, 300);
    CHECK(af.state == SMOOTHAF_VLOG_AF_COOLDOWN && af.armed,
          "improved result enters cooldown while budget remains");
    CHECK(af.episode_result == SMOOTHAF_VLOG_AF_RESULT_IMPROVED,
          "improvement is distinct from physical movement confirmation");
    CHECK(SMOOTHAF_VLOG_AF_MAX_COMMANDS == 4 &&
          SMOOTHAF_VLOG_AF_MAX_REVERSALS == 1,
          "local search is limited to four commands and one reversal");
    CHECK(af.post_move_score == 1100 && af.score_delta == 100,
          "post-move score and delta are recorded");
    CHECK(strcmp(smoothaf_vlog_af_result_name(SMOOTHAF_VLOG_AF_RESULT_IMPROVED),
                 "IMPROVED") == 0,
          "episode result names are available to the menu");
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(!smoothaf_vlog_af_can_command(&af, 0, 0),
          "ACTIVE requires Enable");
    CHECK(smoothaf_vlog_af_can_command(&af, 1, 0),
          "ACTIVE permits command only when enabled");
    smoothaf_vlog_af_record_command(&af, 0);
    smoothaf_vlog_af_stop(&af);
    CHECK(!af.armed && af.state == SMOOTHAF_VLOG_AF_COMPLETE,
          "Stop ends an armed episode");

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

    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 600);
    smoothaf_monitor_accept(&m, &s, 1100);
    smoothaf_monitor_accept(&m, &s, 1600);
    CHECK(m.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR,
          "persistent loss enters possible blur");
    CHECK(m.reference == 1000, "blur does not lower reference");
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, m.score);
    CHECK(smoothaf_vlog_af_should_probe(&af, m.state, SMOOTHAF_VLOG_AF_DRY_RUN),
          "episode arms from the monitor's real blur state");

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
