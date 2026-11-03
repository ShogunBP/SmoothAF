#include <stdio.h>
#include "../src/smoothaf_algorithm.h"
#include "../src/smoothaf_direction.h"
#include "../src/smoothaf_recovery.h"

static int failures;
#define CHECK(condition, message) do { if (!(condition)) { puts(message); failures++; } } while (0)

int main(void)
{
    struct smoothaf_recovery recovery;
    int direction;
    int step;

    CHECK(smoothaf_algorithm_normalize(-1) == SMOOTHAF_ALGORITHM_CURRENT_VLOG,
          "invalid algorithm must use Current Vlog");
    CHECK(smoothaf_algorithm_normalize(4) == SMOOTHAF_ALGORITHM_CURRENT_VLOG,
          "out-of-range algorithm must use Current Vlog");
    CHECK(!smoothaf_algorithm_focus_test_supported(SMOOTHAF_ALGORITHM_CURRENT_VLOG),
          "Focus Test must not advertise Current Vlog");
    CHECK(smoothaf_algorithm_focus_test_supported(SMOOTHAF_ALGORITHM_LEGACY_HC),
          "Focus Test must retain Legacy HC");
    CHECK(smoothaf_vlog_direction_normalize(-1) == SMOOTHAF_VLOG_DIRECTION_AUTO &&
          smoothaf_vlog_direction_normalize(3) == SMOOTHAF_VLOG_DIRECTION_AUTO,
          "invalid Vlog directions must normalize to AUTO");
    CHECK(smoothaf_vlog_direction_migrate(
              SMOOTHAF_VLOG_DIRECTION_AUTO, 0, 0, 0) == SMOOTHAF_VLOG_DIRECTION_AUTO,
          "new Vlog configurations default to AUTO");
    CHECK(smoothaf_vlog_direction_migrate(
              SMOOTHAF_VLOG_DIRECTION_AUTO, 0, 1, 1) == SMOOTHAF_VLOG_DIRECTION_B,
          "explicit legacy B direction is migrated only when Vlog direction is untouched");
    CHECK(smoothaf_vlog_direction_migrate(
              SMOOTHAF_VLOG_DIRECTION_AUTO, 1, 1, 1) == SMOOTHAF_VLOG_DIRECTION_AUTO,
          "explicit AUTO is not overwritten by legacy direction");
    CHECK(smoothaf_vlog_direction_migrate(
              -1, 0, 1, 1) == SMOOTHAF_VLOG_DIRECTION_B,
          "untouched migration sentinel accepts the legacy B direction");
    CHECK(smoothaf_vlog_direction_migrate(
              SMOOTHAF_VLOG_DIRECTION_AUTO, 1, 1, 1) == SMOOTHAF_VLOG_DIRECTION_AUTO,
          "explicit saved AUTO remains authoritative during migration");
    CHECK(smoothaf_vlog_direction_forced_sign(SMOOTHAF_VLOG_DIRECTION_A) == -1 &&
          smoothaf_vlog_direction_forced_sign(SMOOTHAF_VLOG_DIRECTION_B) == 1,
          "A and B retain independently forced initial signs");

    CHECK(!smoothaf_recovery_should_start(1, 0, 1, 0) &&
          smoothaf_recovery_should_start(1, 0, 1, 1),
          "Movie recovery must require explicit opt-in");
    smoothaf_recovery_begin(&recovery, 100, -1);
    CHECK(recovery.phase == SMOOTHAF_RECOVERY_COARSE &&
          smoothaf_recovery_next(&recovery, &direction, &step) &&
          direction == -1 && step == SMOOTHAF_RECOVERY_COARSE_STEP,
          "recovery must start with a bounded coarse move");
    smoothaf_recovery_commit_move(&recovery);
    smoothaf_recovery_on_score(&recovery, 200);
    CHECK(recovery.phase == SMOOTHAF_RECOVERY_BRACKET,
          "recovery must enter bracket phase");
    CHECK(smoothaf_recovery_next(&recovery, &direction, &step) &&
          direction == 1 && step == SMOOTHAF_RECOVERY_COARSE_STEP,
          "recovery must issue a bounded return-to-origin command");
    smoothaf_recovery_commit_move(&recovery);
    smoothaf_recovery_on_score(&recovery, 180);
    CHECK(smoothaf_recovery_next(&recovery, &direction, &step) &&
          direction == 1 && step == SMOOTHAF_RECOVERY_COARSE_STEP,
          "recovery must probe the opposite bracket side");
    smoothaf_recovery_commit_move(&recovery);
    smoothaf_recovery_on_score(&recovery, 170);
    CHECK(smoothaf_recovery_next(&recovery, &direction, &step) &&
          step == SMOOTHAF_RECOVERY_FINE_STEP,
          "recovery must issue a bounded fine probe");
    smoothaf_recovery_cancel(&recovery);
    CHECK(recovery.phase == SMOOTHAF_RECOVERY_ABORTED &&
          recovery.result == SMOOTHAF_RECOVERY_CANCELLED,
          "recovery cancellation must be observable");

    if (failures) return 1;
    puts("SmoothAF algorithm tests passed");
    return 0;
}
