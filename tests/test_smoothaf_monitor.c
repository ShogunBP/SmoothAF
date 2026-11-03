#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_monitor.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void test_monitor_task_state_tracks_lifecycle(void)
{
    CHECK(strcmp(smoothaf_monitor_task_state_name_for(0, 0, 1, 0), "STOPPED") == 0,
          "monitor menu reports STOPPED after task exit");
    CHECK(strcmp(smoothaf_monitor_task_state_name_for(1, 0, 0, 0), "RUNNING") == 0,
          "monitor menu reports RUNNING only while task is running");
    CHECK(strcmp(smoothaf_monitor_task_state_name_for(1, 1, 0, 0), "STOPPING") == 0,
          "monitor menu reports STOPPING during task shutdown");
    CHECK(strcmp(smoothaf_monitor_task_state_name_for(0, 0, 0, 0), "STARTING") == 0,
          "monitor menu reports STARTING during a task handoff");
    CHECK(strcmp(smoothaf_monitor_task_state_name_for(0, 0, 0, 1), "ERROR") == 0,
          "monitor menu reports ERROR when startup or task state fails");
}

static void test_monitor_session_preserves_mode(void)
{
    struct smoothaf_vlog_af af;
    smoothaf_vlog_af_init(&af);
    af.mode = SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS;
    af.continuous_armed = 1;
    smoothaf_vlog_af_reset_session(&af, af.mode);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
          !af.armed && !af.continuous_armed && !af.safety_paused,
          "monitor session reset preserves selected mode but clears authorization");
    smoothaf_vlog_af_reset_session(&af, SMOOTHAF_VLOG_AF_DRY_RUN);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_DRY_RUN && !af.armed,
          "session reset accepts the selected DRY RUN mode");
    smoothaf_vlog_af_reset_session(&af, SMOOTHAF_VLOG_AF_OFF);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_OFF && !af.armed,
          "session reset preserves an explicitly selected OFF mode");
}

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

struct lens_mock
{
    int args[SMOOTHAF_VLOG_AF_MAX_COMMANDS];
    int count;
};

static int mock_lens_focus(struct lens_mock *mock, int direction)
{
    if (mock->count < SMOOTHAF_VLOG_AF_MAX_COMMANDS)
        mock->args[mock->count++] = direction;
    return 1;
}

static void issue_mock_command(struct smoothaf_vlog_af *af,
                               struct lens_mock *mock, uint32_t now)
{
    CHECK(af->state == SMOOTHAF_VLOG_AF_PROBING,
          "mock command is issued only while probing");
    CHECK(smoothaf_vlog_af_can_command(af, 1, now),
          "mock lens command passes the ACTIVE safety gate");
    CHECK(mock_lens_focus(mock, af->direction),
          "mock lens_focus accepts the requested direction");
    smoothaf_vlog_af_record_command(af, now);
    af->current_position += af->direction;
}

static void test_vlog_auto_probe_and_forced_directions(void)
{
    struct smoothaf_vlog_af af;
    int direction;
    int step;
    int algorithm;

    for (algorithm = SMOOTHAF_ALGORITHM_CURRENT_VLOG;
         algorithm <= SMOOTHAF_ALGORITHM_NORMAL_AF; algorithm++)
    {
        smoothaf_vlog_af_init(&af);
        af.algorithm = algorithm;
        smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE,
                                     -1, 1000);
        CHECK(smoothaf_vlog_af_auto_needs_move(
                  &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step) &&
              step == 3,
              "Vlog AUTO is available for every Vlog algorithm");
    }

    smoothaf_vlog_af_init(&af);
    af.algorithm = SMOOTHAF_ALGORITHM_CURRENT_VLOG;
    af.generation = -1; /* deterministic A-side first probe for this seam test */
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE,
                                 -1, 1000);
    CHECK(smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step) &&
          direction == -1 && step == 3,
          "Vlog AUTO probes the first side with the configured step");
    smoothaf_vlog_af_auto_commit_move(&af);
    smoothaf_vlog_af_auto_on_score(&af, 1100);
    CHECK(smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step) &&
          direction == 1,
          "Vlog AUTO returns to origin after the first probe");
    smoothaf_vlog_af_auto_commit_move(&af);
    smoothaf_vlog_af_auto_on_score(&af, 1000);
    CHECK(smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step) &&
          direction == 1,
          "Vlog AUTO probes the opposite side");
    smoothaf_vlog_af_auto_commit_move(&af);
    smoothaf_vlog_af_auto_on_score(&af, 1200);
    CHECK(smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step) &&
          direction == -1,
          "Vlog AUTO returns after the opposite probe");
    smoothaf_vlog_af_auto_commit_move(&af);
    smoothaf_vlog_af_auto_on_score(&af, 1000);
    CHECK(af.auto_probe_done && af.direction == 1 &&
          af.auto_probe_result == SMOOTHAF_AUTO_OPPOSITE_IMPROVED &&
          !smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step),
          "Vlog AUTO selects the better side without a fixed direction");

    smoothaf_vlog_af_init(&af);
    af.algorithm = SMOOTHAF_ALGORITHM_NORMAL_AF;
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE,
                                 -1, 1000);
    CHECK(!smoothaf_vlog_af_auto_needs_move(
              &af, SMOOTHAF_VLOG_DIRECTION_A, 3, &direction, &step),
          "forced A bypasses Vlog AUTO probing");
    af.direction = smoothaf_vlog_direction_forced_sign(
        SMOOTHAF_VLOG_DIRECTION_A);
    CHECK(af.direction == -1,
          "forced A remains the initial Vlog direction");
}

static void test_vlog_auto_probe_reserves_algorithm_budget(void)
{
    struct smoothaf_vlog_af af;
    int direction = -1;
    int step = 3;
    int probe;

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, -1, 1000);
    smoothaf_vlog_af_set_auto_probe(&af, 1);
    smoothaf_vlog_af_set_episode_limit(&af, 1);
    af.state = SMOOTHAF_VLOG_AF_PROBING;

    /* Reproduce the monitor adapter: AUTO commands are real episode commands,
     * but they must not consume the Legacy HC command allowance. */
    for (probe = 0; probe < 4; probe++)
    {
        CHECK(smoothaf_vlog_af_auto_needs_move(
                  &af, SMOOTHAF_VLOG_DIRECTION_AUTO, 3, &direction, &step),
              "integrated AUTO probe requests each bounded move");
        CHECK(smoothaf_vlog_af_can_command(&af, 1, (uint32_t)probe),
              "AUTO probe move passes the physical safety gate");
        smoothaf_vlog_af_record_command(&af, (uint32_t)probe);
        af.current_position += direction * step;
        smoothaf_vlog_af_auto_commit_move(&af);
        af.awaiting_result = 0;
        af.state = SMOOTHAF_VLOG_AF_PROBING;
        smoothaf_vlog_af_auto_on_score(&af, 1000);
    }

    CHECK(af.auto_probe_done && af.commands == 4 &&
          af.current_position == 0,
          "AUTO probe returns to origin without losing its command trace");
    CHECK(smoothaf_vlog_af_can_command(&af, 1, 4),
          "Legacy HC remains eligible after the four-command AUTO probe");
    CHECK(smoothaf_vlog_af_commands_available(&af) >= 1,
          "algorithm budget remains after AUTO probing");
    smoothaf_vlog_af_record_command(&af, 4);
    af.current_position += direction * step;
    {
        int result = smoothaf_vlog_af_record_result(&af, 1100, 124);
        CHECK(result, "first algorithm result is accepted after AUTO");
        CHECK(af.episode_result == SMOOTHAF_VLOG_AF_RESULT_SEARCH_LIMIT,
              "configured one-command algorithm limit is enforced after AUTO");
        CHECK(af.algorithm_commands == 1,
              "first algorithm command is counted separately from AUTO");
    }
}

static void test_direction_is_physical_and_independent(void)
{
    struct smoothaf_vlog_af af;
    struct lens_mock mock;
    int diagnostic_direction;
    int episode;

    memset(&mock, 0, sizeof(mock));
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, -1, 1000);
    diagnostic_direction = 1; /* changing Diagnostic A/B must not rewrite af.direction */
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    issue_mock_command(&af, &mock, 0);
    CHECK(mock.args[0] == -1 && af.current_position == -1,
          "automatic ACTIVE sends its independent initial direction to lens_focus");
    diagnostic_direction = -1;
    (void)diagnostic_direction;
    af.awaiting_result = 0;
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(af.direction == -1,
          "Diagnostic A/B changes do not alter an active automatic direction");

    /* A WORSE result must reverse the next physical request, not just telemetry. */
    smoothaf_vlog_af_record_result(&af, 900, 120);
    CHECK(af.episode_result == SMOOTHAF_VLOG_AF_RESULT_WORSE &&
          af.direction == 1 && af.reversals == 1,
          "WORSE reverses the bounded automatic direction");
    issue_mock_command(&af, &mock, 200);
    CHECK(mock.args[1] == 1,
          "WORSE reversal reaches the next lens_focus direction argument");

    af.awaiting_result = 0;
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    smoothaf_vlog_af_record_result(&af, 800, 320);
    CHECK(!af.armed && af.state == SMOOTHAF_VLOG_AF_COMPLETE &&
          af.reversals == SMOOTHAF_VLOG_AF_MAX_REVERSALS,
          "repeated WORSE terminates at the reversal limit");
    CHECK(mock.count == 2,
          "repeated WORSE cannot create an unbounded command loop");

    /* A new continuous episode gets the automatic source again; it does not
     * inherit a previous episode's reversal direction. */
    memset(&mock, 0, sizeof(mock));
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, -1, 1000);
    for (episode = 0; episode < 3; episode++)
    {
        smoothaf_vlog_af_start_continuous_episode(&af,
                                                  episode & 1 ? 1 : -1,
                                                  1000);
        CHECK(af.direction == (episode & 1 ? 1 : -1),
              "continuous episode initializes from its automatic direction source");
        af.state = SMOOTHAF_VLOG_AF_PROBING;
        issue_mock_command(&af, &mock, 1000 + (uint32_t)episode);
        CHECK(mock.args[mock.count - 1] == (episode & 1 ? 1 : -1),
              "continuous episode sends its selected direction physically");
        af.awaiting_result = 0;
        smoothaf_vlog_af_stop(&af);
        if (episode != 2)
        {
            af.safety_paused = 0;
            af.continuous_armed = 1;
        }
    }

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    smoothaf_vlog_af_pause_continuous(&af, SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN, 0);
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 1) &&
          af.episode_result == SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "UNCERTAIN blocks another physical command");

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    issue_mock_command(&af, &mock, 10);
    smoothaf_vlog_af_stop(&af);
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 11),
          "STOP blocks commands during a pending reversal path");
}

static void test_vlog_af_lifecycle(void)
{
    struct smoothaf_vlog_af af;
    uint32_t commands;
    int position;
    int best;

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1000);
    CHECK(af.armed && af.state == SMOOTHAF_VLOG_AF_WAITING,
          "episode starts armed and waiting");
    CHECK(!smoothaf_vlog_af_advance_cooldown(&af, 5000),
          "cooldown update does not disturb WAITING");
    CHECK(af.state == SMOOTHAF_VLOG_AF_WAITING,
          "WAITING stays WAITING without blur evidence");

    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(smoothaf_vlog_af_can_command(&af, 0, 1000),
          "DRY RUN does not require Enable");
    commands = af.commands;
    position = af.current_position;
    best = af.best;
    CHECK(smoothaf_vlog_af_dry_run_request(&af, 0, 1000),
          "DRY RUN records a decision");
    CHECK(af.state == SMOOTHAF_VLOG_AF_SETTLING && af.awaiting_result &&
          af.settle_until_ms == 1120 && af.commands == commands + 1,
          "allowed decision enters SETTLING with a settle deadline");
    CHECK(af.current_position == position && af.best == best,
          "DRY RUN does not move or change the best position");
    CHECK(!smoothaf_vlog_af_result_due(&af, 1119) &&
          smoothaf_vlog_af_result_due(&af, 1120),
          "SETTLING waits for the configured settle time");

    smoothaf_vlog_af_record_result(&af, 1100, 1120);
    CHECK(af.state == SMOOTHAF_VLOG_AF_COOLDOWN && af.armed,
          "IMPROVED enters COOLDOWN");
    CHECK(af.cooldown_until_ms == 2120,
          "COOLDOWN uses the configured duration");
    best = af.best;
    CHECK(!smoothaf_vlog_af_advance_cooldown(&af, 2119),
          "COOLDOWN does not finish early");
    CHECK(!smoothaf_vlog_af_can_command(&af, 0, 2119),
          "COOLDOWN blocks commands before its deadline");
    CHECK(smoothaf_vlog_af_advance_cooldown(&af, 2120),
          "COOLDOWN finishes at its deadline");
    CHECK(af.state == SMOOTHAF_VLOG_AF_PROBING,
          "finished COOLDOWN returns to PROBING");
    CHECK(af.commands == commands + 1 && af.current_position == position &&
          af.best == best,
          "finishing COOLDOWN does not command or move the lens");
    CHECK(smoothaf_vlog_af_can_command(&af, 0, 2120),
          "PROBING permits the next decision after COOLDOWN");
    CHECK(smoothaf_vlog_af_dry_run_request(&af, 0, 2120) &&
          af.commands == commands + 2 &&
          af.state == SMOOTHAF_VLOG_AF_SETTLING,
          "next decision is recorded only after COOLDOWN");

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(!smoothaf_vlog_af_can_command(&af, 0, 0),
          "ACTIVE requires Enable");
    CHECK(smoothaf_vlog_af_can_command(&af, 1, 0),
          "ACTIVE permits a command when enabled");
    af.armed = 0;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 0),
          "disarmed ACTIVE episode cannot command");
    af.armed = 1;
    af.awaiting_result = 1;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 0),
          "awaiting result blocks another command");
    af.awaiting_result = 0;
    af.commands = SMOOTHAF_VLOG_AF_MAX_COMMANDS;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 0),
          "episode command limit is enforced");
    af.commands = 0;
    af.session_commands = SMOOTHAF_VLOG_AF_MAX_COMMANDS;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 0),
          "session command limit is enforced");
    af.session_commands = 0;
    af.cooldown_until_ms = 500;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 499) &&
          smoothaf_vlog_af_can_command(&af, 1, 500),
          "cooldown deadline blocks commands without changing state");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    smoothaf_vlog_af_record_command(&af, 1000);
    CHECK(!smoothaf_vlog_af_timeout_due(&af, 6000),
          "timeout does not fire at the exact limit");
    CHECK(smoothaf_vlog_af_timeout_due(&af, 6001),
          "timeout fires during SETTLING");
    smoothaf_vlog_af_stop(&af);
    CHECK(!smoothaf_vlog_af_timeout_due(&af, 9000),
          "stopped episode is not timed out");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    CHECK(!smoothaf_vlog_af_should_cancel(&af, SMOOTHAF_VLOG_AF_ACTIVE),
          "active episode is not cancelled while the mode stays ACTIVE");
    CHECK(smoothaf_vlog_af_should_cancel(&af, SMOOTHAF_VLOG_AF_OFF),
          "switching the mode to OFF cancels an armed episode");
    CHECK(!smoothaf_vlog_af_uses_dry_run(&af, SMOOTHAF_VLOG_AF_ACTIVE),
          "ACTIVE episode may use physical commands while the mode is ACTIVE");
    CHECK(smoothaf_vlog_af_uses_dry_run(&af, SMOOTHAF_VLOG_AF_DRY_RUN),
          "switching an ACTIVE episode to DRY RUN stops physical commands");
    smoothaf_vlog_af_stop(&af);
    CHECK(!smoothaf_vlog_af_should_cancel(&af, SMOOTHAF_VLOG_AF_OFF),
          "stopped episode ignores a later OFF switch");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1000);
    CHECK(smoothaf_vlog_af_uses_dry_run(&af, SMOOTHAF_VLOG_AF_DRY_RUN),
          "DRY RUN episode is dry");
    CHECK(smoothaf_vlog_af_uses_dry_run(&af, SMOOTHAF_VLOG_AF_ACTIVE),
          "DRY RUN episode cannot become physical after a mode change");

    {
        const int states[] = {
            SMOOTHAF_VLOG_AF_PROBING,
            SMOOTHAF_VLOG_AF_SETTLING,
            SMOOTHAF_VLOG_AF_COOLDOWN,
        };
        size_t i;
        for (i = 0; i < sizeof(states) / sizeof(states[0]); i++)
        {
            smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
            af.state = states[i];
            if (states[i] == SMOOTHAF_VLOG_AF_SETTLING) af.awaiting_result = 1;
            smoothaf_vlog_af_stop(&af);
            CHECK(!af.armed && af.state == SMOOTHAF_VLOG_AF_COMPLETE &&
                  !smoothaf_vlog_af_can_command(&af, 1, 9999),
                  "STOP ends PROBING, SETTLING and COOLDOWN episodes");
        }
    }
}

static void test_movie_recording_policy(void)
{
    CHECK(!smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 0, 1, 1, 1, 0, 0),
          "Movie ACTIVE is blocked unless experimental authorization is ON");
    CHECK(smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 0) &&
          smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 1, 1, 1, 0, 0),
          "authorized ACTIVE modes permit normal H.264 Movie recording");
    CHECK(!smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 0, 0, 0),
           "authorized ACTIVE remains blocked before H.264 recording starts");
    CHECK(!smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 1, 0) &&
          !smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 1),
          "RAW and custom recording remain blocked");
    CHECK(!smoothaf_movie_active_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 0, 1, 0, 0),
          "H.264 recording still requires Movie mode");
    CHECK(!smoothaf_movie_active_gate_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 0,
              0, 1, 0, 0, 0, 0) &&
          !smoothaf_movie_active_gate_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 0,
              0, 0, 0, 1, 0, 0),
          "MF, Follow Focus, Rack Focus and stacking remain safety gates");
    CHECK(smoothaf_movie_active_gate_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 1, 1, 1, 0, 0,
              0, 0, 0, 0, 0, 0),
          "authorized ACTIVE gate accepts compatible H.264 state");
    CHECK(!smoothaf_movie_active_gate_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 0,
              1, 0, 0, 0, 0, 0) &&
          !smoothaf_movie_active_gate_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 1, 0, 0,
              0, 0, 1, 0, 0, 0),
          "cancel and Continuous AF remain blocked for Movie Active");

    CHECK(smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0),
          "H.264 Movie Dry Run is explicitly allowed");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 1, 0, 0),
          "Movie ACTIVE remains unavailable during Dry Run validation");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 0, 1, 1, 0, 0),
          "Movie Dry Run requires LiveView");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 0, 1, 0, 0),
          "Movie Dry Run requires Movie mode");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 0, 0, 0),
          "Movie Dry Run requires recording");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 1, 0),
          "custom recording remains blocked");
    CHECK(!smoothaf_movie_dry_run_recording_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 1),
          "RAW recording remains blocked");
    CHECK(smoothaf_movie_dry_run_gate_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0,
              0, 0, 0, 0, 0, 0),
          "Movie Dry Run safety gates are clear");
    CHECK(!smoothaf_movie_dry_run_gate_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0,
              1, 0, 0, 0, 0, 0) &&
          !smoothaf_movie_dry_run_gate_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0,
              0, 1, 0, 0, 0, 0) &&
          !smoothaf_movie_dry_run_gate_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0,
              0, 0, 1, 0, 0, 0) &&
          !smoothaf_movie_dry_run_gate_allowed(
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0, 0,
              0, 0, 0, 1, 0, 0),
          "cancel, MF, Continuous AF and Follow Focus remain blocked");
    CHECK(smoothaf_movie_recording_class(1, 0, 0) == SMOOTHAF_MOVIE_RECORDING_H264 &&
          smoothaf_movie_recording_class(0, 1, 0) == SMOOTHAF_MOVIE_RECORDING_CUSTOM &&
          smoothaf_movie_recording_class(0, 0, 1) == SMOOTHAF_MOVIE_RECORDING_RAW &&
          smoothaf_movie_recording_class(0, 0, 0) == SMOOTHAF_MOVIE_RECORDING_NONE,
          "recording classes distinguish H.264, custom, RAW and idle");
    {
        struct smoothaf_movie_recording_state recording;
        smoothaf_movie_recording_init(&recording);
        CHECK(smoothaf_movie_recording_observe(
                  &recording, SMOOTHAF_MOVIE_RECORDING_H264, 100) == 1 &&
              recording.active && recording.started_ms == 100,
              "recording start edge is observable");
        recording.samples = 3;
        recording.valid = 2;
        recording.invalid = 1;
        CHECK(smoothaf_movie_recording_observe(
                  &recording, SMOOTHAF_MOVIE_RECORDING_H264, 150) == 0,
              "steady recording has no repeated edge");
        CHECK(smoothaf_movie_recording_observe(
                  &recording, SMOOTHAF_MOVIE_RECORDING_NONE, 200) == 2 &&
              !recording.active && recording.ended_ms == 200 &&
              recording.samples == 3 && recording.valid == 2,
              "recording end edge preserves RAM counters");
    }
}

static void test_continuous_active(void)
{
    struct smoothaf_vlog_af af;
    int episode;

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS &&
          af.continuous_armed && !af.armed,
          "continuous ACTIVE requires one persistent manual arm");
    CHECK(!smoothaf_vlog_af_should_resume_continuous(
              &af, SMOOTHAF_MONITOR_STABLE, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "continuous ACTIVE does not move without confirmed blur");

    for (episode = 0; episode < 3; episode++)
    {
        smoothaf_vlog_af_update_continuous_latch(
            &af, SMOOTHAF_MONITOR_STABLE, (uint32_t)(100 + episode * 3000));
        CHECK(smoothaf_vlog_af_should_resume_continuous(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(100 + episode * 3000)),
              "confirmed blur starts another continuous episode");
        smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
        af.state = SMOOTHAF_VLOG_AF_PROBING;
        CHECK(smoothaf_vlog_af_can_command(&af, 1,
                                           (uint32_t)(100 + episode * 3000)),
              "continuous episode allows a gated command");
        smoothaf_vlog_af_record_command(&af,
                                        (uint32_t)(100 + episode * 3000));
        af.current_position++;
        CHECK(smoothaf_vlog_af_result_due(&af,
                                          (uint32_t)(220 + episode * 3000)),
              "continuous episode waits for post-move measurement");
        smoothaf_vlog_af_record_result(&af, 1100,
                                       (uint32_t)(220 + episode * 3000));
        CHECK(af.episodes_completed == (uint32_t)(episode + 1) &&
              !af.armed && af.continuous_armed,
              "continuous episode completes without cancelling authorization");
        CHECK(af.episode_result == SMOOTHAF_VLOG_AF_RESULT_IMPROVED,
              "partial improvement remains distinct from convergence");
        CHECK(!smoothaf_vlog_af_should_resume_continuous(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(220 + episode * 3000)),
              "cooldown blocks an immediate retrigger");
        CHECK(smoothaf_vlog_af_should_resume_continuous(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(1720 + episode * 3000)),
              "persistent blur recheck is eligible after its deadline");
        CHECK(smoothaf_vlog_af_next_episode_block_reason(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(719 + episode * 3000)) ==
                  SMOOTHAF_VLOG_AF_NEXT_BLOCK_COOLDOWN,
              "next-episode diagnostics identify an unexpired cooldown");
        CHECK(smoothaf_vlog_af_cooldown_remaining(
                  &af, (uint32_t)(1219 + episode * 3000)) == 1 &&
              smoothaf_vlog_af_cooldown_remaining(
                  &af, (uint32_t)(1220 + episode * 3000)) == 0,
              "continuous cooldown reports its exact expiry boundary");
        af.cooldown_next_episode = 0;
        CHECK(smoothaf_vlog_af_next_episode_block_reason(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(719 + episode * 3000)) ==
                  SMOOTHAF_VLOG_AF_NEXT_BLOCK_BLUR_RECHECK,
              "next-episode diagnostics identify an unexpired blur recheck");
        smoothaf_vlog_af_update_continuous_latch(
            &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
            (uint32_t)(1220 + episode * 3000));
        CHECK(smoothaf_vlog_af_should_resume_continuous(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1,
                  (uint32_t)(1220 + episode * 3000)),
              "persistent blur recheck permits a gated retrigger after cooldown");
        smoothaf_vlog_af_update_continuous_latch(
            &af, SMOOTHAF_MONITOR_STABLE,
            (uint32_t)(1221 + episode * 3000));
    }
    CHECK(af.session_commands == 3,
          "continuous session telemetry accumulates across episodes");

    /* Controlled-clock endurance: no terminal COMPLETE state may erase the
     * durable authorization, even after a simulated run beyond two minutes. */
    smoothaf_vlog_af_update_continuous_latch(&af, SMOOTHAF_MONITOR_STABLE, 120000);
    for (episode = 0; episode < 3; episode++)
    {
        uint32_t now = 120000 + (uint32_t)episode * 3000;
        af.cooldown_next_episode = 0;
        af.wait_for_blur_clear = 0;
        CHECK(smoothaf_vlog_af_should_resume_continuous(
                  &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
                  SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, now),
              "long controlled-clock run remains eligible for later episodes");
        smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
        af.state = SMOOTHAF_VLOG_AF_PROBING;
        smoothaf_vlog_af_record_command(&af, now);
        smoothaf_vlog_af_record_result(&af, 1100, now + 120);
        CHECK(af.continuous_armed && !af.armed,
              "long controlled-clock episode completes without terminalizing session");
    }
    CHECK(af.episodes_completed >= 6 && af.continuous_armed,
          "controlled-clock run beyond 120 seconds preserves continuous authorization");
    smoothaf_vlog_af_update_continuous_latch(&af, SMOOTHAF_MONITOR_STABLE, 10000);
    smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
    smoothaf_vlog_af_set_episode_limit(&af, 1);
    CHECK(af.episode_limit == 1 &&
          smoothaf_vlog_af_commands_available(&af) == 1,
          "configured episode limit applies to the next episode");
    smoothaf_vlog_af_set_episode_limit(&af, 0);
    CHECK(af.episode_limit == SMOOTHAF_VLOG_AF_DEFAULT_EPISODE_LIMIT,
          "invalid episode limit falls back to the default");
    smoothaf_vlog_af_set_episode_limit(&af, 1);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    smoothaf_vlog_af_record_command(&af, 10000);
    af.awaiting_result = 0;
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 10100),
          "per-episode command budget remains enforced");

    smoothaf_vlog_af_pause_continuous(&af, SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
                                      10100);
    CHECK(af.continuous_armed && !af.armed && af.wait_for_blur_clear,
          "invalid post-move score pauses without cancelling consent");
    smoothaf_vlog_af_stop(&af);
    CHECK(!af.continuous_armed && af.safety_paused && !af.armed,
          "STOP permanently disarms continuous authorization");
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    af.safety_paused = 1;
    CHECK(!smoothaf_vlog_af_can_command(&af, 1, 0),
          "safety pause blocks a continuous command");
    CHECK(!smoothaf_vlog_af_should_resume_continuous(
              &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "safety pause blocks automatic episode creation");

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    CHECK(!smoothaf_vlog_af_should_resume_continuous(
              &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
              SMOOTHAF_VLOG_AF_ACTIVE, 0, 0) &&
          !smoothaf_vlog_af_should_resume_continuous(
              &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
              SMOOTHAF_VLOG_AF_DRY_RUN, 1, 0) &&
          !smoothaf_vlog_af_should_resume_continuous(
              &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
              SMOOTHAF_VLOG_AF_OFF, 1, 0),
          "continuous auto-start requires ACTIVE, Enable, and blur gates");
    smoothaf_vlog_af_pause_continuous(&af, SMOOTHAF_VLOG_AF_RESULT_FAILED, 0);
    smoothaf_vlog_af_update_continuous_latch(&af, SMOOTHAF_MONITOR_STABLE, 2000);
    smoothaf_vlog_af_pause_continuous(&af, SMOOTHAF_VLOG_AF_RESULT_FAILED, 2000);
    CHECK(af.consecutive_failures == SMOOTHAF_VLOG_AF_MAX_CONSECUTIVE_FAILURES &&
          af.safety_paused,
          "two consecutive failures latch the continuous safety pause");

    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    smoothaf_vlog_af_start_continuous_episode(&af, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    smoothaf_vlog_af_record_command(&af, 0);
    CHECK(smoothaf_vlog_af_timeout_due(&af, SMOOTHAF_VLOG_AF_TIMEOUT_MS + 1),
          "timeout works when the first command occurs at clock zero");
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1000);
    CHECK(!smoothaf_vlog_af_should_cancel(&af, SMOOTHAF_VLOG_AF_DRY_RUN),
          "DRY RUN episode remains dry instead of self-cancelling");
}

static void start_qualified_monitor(struct smoothaf_monitor *m, int score,
                                    int mean)
{
    struct smoothaf_monitor_sample s = sample(score, mean, 1, 1);
    smoothaf_monitor_init(m, SMOOTHAF_MONITOR_METRIC_SAD,
                          SMOOTHAF_MONITOR_INTERVAL_500);
    smoothaf_monitor_start(m, SMOOTHAF_MONITOR_METRIC_SAD, 0);
    smoothaf_monitor_accept(m, &s, 10);
    smoothaf_monitor_qualify_reference(m, score, mean,
                                       SMOOTHAF_MONITOR_METRIC_SAD, 20);
}

static void test_reference_provisional_blocks_blur(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s = sample(1000, 120, 1, 1);
    smoothaf_monitor_init(&m, SMOOTHAF_MONITOR_METRIC_SAD,
                          SMOOTHAF_MONITOR_INTERVAL_500);
    smoothaf_monitor_start(&m, SMOOTHAF_MONITOR_METRIC_SAD, 0);
    smoothaf_monitor_accept(&m, &s, 10);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL &&
          m.have_reference && !m.possible_blur,
          "initialization creates a provisional reference");
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    smoothaf_monitor_accept(&m, &s, 200);
    CHECK(!m.possible_blur && m.state != SMOOTHAF_MONITOR_POSSIBLE_BLUR &&
          m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL,
          "an unqualified initial scene cannot declare blur");
}

static void test_recalibration_requires_af_evidence(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    start_qualified_monitor(&m, 1000, 120);
    s = sample(1000, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    smoothaf_monitor_accept(&m, &s, 200);
    smoothaf_monitor_accept(&m, &s, 300);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_RECALIBRATE &&
          !m.have_reference,
          "confirmed lighting change invalidates the old reference");
    s = sample(100, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 400);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL &&
          m.reference == 100 && !m.possible_blur,
          "the first post-recalibration sample is provisional, not focus evidence");
    s = sample(60, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 500);
    smoothaf_monitor_accept(&m, &s, 600);
    CHECK(!m.possible_blur &&
          m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL,
          "a defocused new scene is not promoted to a qualified reference");
    smoothaf_monitor_qualify_reference(&m, 900, 200,
                                       SMOOTHAF_MONITOR_METRIC_SAD, 700);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID && m.reference == 900,
          "bounded AF evidence promotes the recalibrated scene");
}

static void test_lighting_change_suppresses_defocus_decision(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    start_qualified_monitor(&m, 1000, 120);
    /* Exposure and apparent sharpness change together: this is a new scene,
     * not proof that the old subject went out of focus. */
    s = sample(100, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    smoothaf_monitor_accept(&m, &s, 200);
    smoothaf_monitor_accept(&m, &s, 300);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_RECALIBRATE &&
          !m.have_reference && !m.possible_blur,
          "a combined scene and exposure change recalibrates without declaring blur");
    s = sample(80, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 400);
    smoothaf_monitor_accept(&m, &s, 500);
    smoothaf_monitor_accept(&m, &s, 600);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL &&
          !m.possible_blur,
          "the new scene stays provisional until AF provides evidence");
}

static void test_blur_confirmation_is_consecutive(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    start_qualified_monitor(&m, 1000, 120);
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    smoothaf_monitor_accept(&m, &s, 200);
    CHECK(!m.possible_blur && m.blur_bad_count == 1,
          "first blurred sample is not enough");
    s = sample(1300, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 300);
    CHECK(!m.possible_blur && m.blur_bad_count == 0,
          "middle-band sample breaks blur consecutiveness");
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 400);
    CHECK(!m.possible_blur && m.blur_bad_count == 1,
          "low / middle / low does not confirm blur");
    smoothaf_monitor_accept(&m, &s, 500);
    CHECK(m.possible_blur && m.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR,
          "two genuinely consecutive low samples still confirm blur");
}

static void test_single_lighting_oscillation_preserves_reference(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    int epoch;
    int i;
    start_qualified_monitor(&m, 1000, 120);
    epoch = m.reference_epoch;
    s = sample(1000, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.lighting_count == 1 && !m.possible_blur,
          "one lighting excursion does not invalidate the reference");
    s = sample(1000, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 200);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.lighting_count == 0 && m.state == SMOOTHAF_MONITOR_STABLE,
          "returning to the original exposure restores decisions without churn");

    for (i = 0; i < 8; i++)
    {
        s = sample(1000, (i & 1) ? 120 : 200, 1, 1);
        smoothaf_monitor_accept(&m, &s, (uint32_t)(300 + i * 100));
        CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID,
              "periodic flicker never invalidates a qualified reference");
    }
    CHECK(m.reference_epoch == epoch && m.have_reference &&
          !m.possible_blur,
          "flicker does not create new reference epochs or false blur");
}

static void test_low_texture_does_not_touch_reference(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    start_qualified_monitor(&m, 1000, 120);
    s = sample(20, 120, 1, 0);
    smoothaf_monitor_accept(&m, &s, 100);
    CHECK(m.state == SMOOTHAF_MONITOR_UNCERTAIN && !m.possible_blur &&
          m.reference == 1000 &&
          m.reference_state == SMOOTHAF_MONITOR_REF_VALID,
          "a textureless sample neither changes the reference nor declares blur");
    smoothaf_monitor_accept(&m, &s, 200);
    smoothaf_monitor_accept(&m, &s, 300);
    CHECK(!m.possible_blur && m.reference == 1000 &&
          m.metrics.low_confidence == 3,
          "persistent low texture stays uncertain instead of forcing blur");
    s = sample(1000, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 400);
    smoothaf_monitor_accept(&m, &s, 500);
    CHECK(m.state == SMOOTHAF_MONITOR_STABLE &&
          m.reference_state == SMOOTHAF_MONITOR_REF_VALID,
          "confidence returning restores decisions on the same reference");
}

static void test_low_confidence_breaks_blur_consecutiveness(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    start_qualified_monitor(&m, 1000, 120);
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 100);
    smoothaf_monitor_accept(&m, &s, 200);
    CHECK(m.blur_bad_count == 1 && !m.possible_blur,
          "confident blurred samples start a run");
    s = sample(100, 120, 1, 0);
    smoothaf_monitor_accept(&m, &s, 300);
    CHECK(m.blur_bad_count == 0 && !m.possible_blur,
          "an unconfident sample breaks the blur run");
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 400);
    CHECK(m.blur_bad_count == 1 && !m.possible_blur,
          "the next low sample starts a fresh run instead of completing one");
    smoothaf_monitor_accept(&m, &s, 500);
    CHECK(m.possible_blur,
          "two genuinely adjacent confident low samples still confirm blur");
}

static void test_post_move_lighting_is_observable(void)
{
    struct smoothaf_monitor m;
    int reference;
    int reference_mean;
    start_qualified_monitor(&m, 1000, 120);
    reference = m.reference;
    reference_mean = m.reference_mean;
    CHECK(!smoothaf_monitor_lighting_changed(&m, 140),
          "small exposure noise is not a lighting change");
    CHECK(smoothaf_monitor_lighting_changed(&m, 152),
          "a post-move lighting change is detectable without moving the lens");
    CHECK(m.reference == reference && m.reference_mean == reference_mean,
          "observing post-move lighting does not mutate the reference");
}

static void test_initial_acquisition_policy(void)
{
    struct smoothaf_vlog_af af;
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    CHECK(smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "a provisional reference authorizes one bounded acquisition");
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 0, 0),
          "Enable OFF blocks initial acquisition");
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE, 1, 0),
          "initial acquisition is specific to ACTIVE CONTINUOUS");
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_VALID, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "a qualified reference does not trigger initial acquisition");
    af.acquisition_epoch = 1;
    af.acquisition_attempts = SMOOTHAF_VLOG_AF_MAX_ACQUISITION_ATTEMPTS;
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "an exhausted provisional epoch cannot repeat acquisition");
    CHECK(smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 2,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "a new scene epoch authorizes a new bounded acquisition");
    CHECK(smoothaf_vlog_af_next_episode_block_reason_for(
              &af, SMOOTHAF_MONITOR_STABLE, SMOOTHAF_MONITOR_REF_PROVISIONAL,
              1, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0) ==
              SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_EXHAUSTED,
          "next-episode diagnostics expose exhausted acquisition attempts");
    af.acquisition_epoch = -1;
    af.safety_paused = 1;
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0),
          "SAFETY_PAUSED blocks initial acquisition");
    af.safety_paused = 0;
    af.acquisition_attempts = 0;
    af.cooldown_next_episode = 1;
    af.cooldown_until_ms = 1000;
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 500) &&
          smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 1000),
          "cooldown bounds initial acquisition timing");
    smoothaf_vlog_af_begin_acquisition(&af, 1);
    CHECK(af.acquisition_attempts == 1 && af.session_acquisitions == 1,
          "initial acquisition records a bounded attempt");
    smoothaf_vlog_af_note_acquisition_failure(&af, 1000);
    CHECK(!smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 2500),
          "acquisition backoff blocks an immediate retry");
    af.cooldown_next_episode = 0;
    af.wait_for_blur_clear = 0;
    CHECK(smoothaf_vlog_af_should_acquire_continuous(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1,
              SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 3000),
          "a failed acquisition can retry after backoff");
    af.acquisition_attempts = SMOOTHAF_VLOG_AF_MAX_ACQUISITION_ATTEMPTS;
    CHECK(smoothaf_vlog_af_acquisition_exhausted(
              &af, SMOOTHAF_MONITOR_REF_PROVISIONAL, 1),
          "acquisition attempts are explicitly exhausted");
    CHECK(smoothaf_vlog_af_next_episode_block_reason_for(
              &af, SMOOTHAF_MONITOR_STABLE, SMOOTHAF_MONITOR_REF_PROVISIONAL,
              1, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 3000) ==
              SMOOTHAF_VLOG_AF_NEXT_BLOCK_ACQUIRE_EXHAUSTED,
          "telemetry distinguishes acquisition exhaustion");
    af.cooldown_next_episode = 0;
    af.acquisition_attempts = 0;
    CHECK(smoothaf_vlog_af_next_episode_block_reason_for(
              &af, SMOOTHAF_MONITOR_STABLE, SMOOTHAF_MONITOR_REF_RECALIBRATE,
              1, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0) ==
              SMOOTHAF_VLOG_AF_NEXT_BLOCK_REFERENCE,
          "a missing reference is reported distinctly");
    CHECK(smoothaf_vlog_af_next_episode_block_reason_for(
              &af, SMOOTHAF_MONITOR_STABLE, SMOOTHAF_MONITOR_REF_VALID,
              1, SMOOTHAF_VLOG_AF_ACTIVE_CONTINUOUS, 1, 0) ==
              SMOOTHAF_VLOG_AF_NEXT_BLOCK_MONITOR,
          "a qualified reference still needs confirmed blur");
}

static void test_environmental_abort_is_not_af_failure(void)
{
    struct smoothaf_vlog_af af;
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_arm_continuous(&af, 1, 1000);
    af.armed = 1;
    af.awaiting_result = 1;
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    af.commands = 2;
    smoothaf_vlog_af_abort_environmental(&af, 1000);
    CHECK(!af.armed && af.continuous_armed &&
          af.episode_result == SMOOTHAF_VLOG_AF_RESULT_UNCERTAIN,
          "environmental abort finishes the episode without disarming continuous");
    CHECK(af.consecutive_failures == 0 && !af.safety_paused,
          "a lighting change is not counted as an AF failure");
    CHECK(af.cooldown_next_episode && af.wait_for_blur_clear &&
          af.commands == 2,
          "environmental abort preserves command accounting and safety timing");
}

static void test_reference_qualification_criteria(void)
{
    struct smoothaf_vlog_af af;
    smoothaf_vlog_af_init(&af);
    CHECK(smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_CONVERGED, 0, SMOOTHAF_AUTO_INVALID,
              2, 1300, 1000, 1300, 1, 1),
          "a converged bounded search with a representative final score qualifies");
    CHECK(smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_BEST_RECOVERED, 0, SMOOTHAF_AUTO_INVALID,
              2, 1300, 1000, 1300, 1, 1),
          "a photometrically recovered best position qualifies");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_UNCERTAIN, 1, 1000, 1000, 1000, 1, 1),
          "AUTO uncertainty never qualifies a provisional reference");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1010, 1000, 1010, 1, 1),
          "small improvement does not qualify a flat response");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1300, 1000, 1300, 1, 1),
          "NO_IMPROVEMENT never substitutes for confirmed convergence");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1300, 1000, 1000, 1, 1),
          "a final score far below the best does not qualify");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1300, 1000, 1300, 0, 1),
          "an invalid post-move result does not qualify");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_NO_IMPROVEMENT, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1300, 1000, 1300, 1, 0),
          "environmentally unstable AUTO samples do not qualify");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_IMPROVED, 1,
              SMOOTHAF_AUTO_FIRST_IMPROVED, 1, 1300, 1000, 1300, 1, 1),
          "partial improvement alone is not treated as focus confirmation");
    CHECK(smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_CONVERGED, 0, SMOOTHAF_AUTO_INVALID,
              2, 1300, 1000, 1300, 1, 1),
          "a confirmed convergence promotes a valid reference");
    CHECK(!smoothaf_vlog_af_result_qualifies_reference(
              SMOOTHAF_VLOG_AF_RESULT_CONVERGED, 0, SMOOTHAF_AUTO_INVALID,
              2, 1300, 1000, 1000, 1, 1),
          "a converged result with a poor final score does not promote");
    smoothaf_vlog_af_auto_begin_measurement(&af, 100);
    smoothaf_vlog_af_auto_observe_mean(&af, 120);
    smoothaf_vlog_af_auto_observe_mean(&af, 140);
    CHECK(!af.auto_probe_lighting_valid &&
          af.auto_probe_max_mean - af.auto_probe_min_mean >=
          SMOOTHAF_MONITOR_LIGHT_DELTA,
          "gradual AUTO lighting drift invalidates score comparison");
}

static void test_manual_provisional_probe_is_bounded(void)
{
    struct smoothaf_vlog_af af;
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    CHECK(smoothaf_vlog_af_should_probe_reference(
              &af, SMOOTHAF_MONITOR_STABLE,
              SMOOTHAF_MONITOR_REF_PROVISIONAL, SMOOTHAF_VLOG_AF_ACTIVE),
          "manual arming authorizes one provisional acquisition");
    CHECK(smoothaf_vlog_af_should_probe_reference(
              &af, SMOOTHAF_MONITOR_POSSIBLE_BLUR,
              SMOOTHAF_MONITOR_REF_VALID, SMOOTHAF_VLOG_AF_ACTIVE),
          "confirmed blur still promotes a manual episode");
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(!smoothaf_vlog_af_should_probe_reference(
              &af, SMOOTHAF_MONITOR_STABLE,
              SMOOTHAF_MONITOR_REF_PROVISIONAL, SMOOTHAF_VLOG_AF_ACTIVE),
          "a probing episode is not restarted by the acquisition gate");
    af.state = SMOOTHAF_VLOG_AF_WAITING;
    af.armed = 0;
    CHECK(!smoothaf_vlog_af_should_probe_reference(
              &af, SMOOTHAF_MONITOR_STABLE,
              SMOOTHAF_MONITOR_REF_PROVISIONAL, SMOOTHAF_VLOG_AF_ACTIVE),
          "a disarmed episode cannot acquire");
}

int main(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    int from;
    int to;
    struct smoothaf_vlog_af af;

    test_monitor_task_state_tracks_lifecycle();
    test_vlog_af_lifecycle();
    test_movie_recording_policy();
    test_continuous_active();
    test_vlog_auto_probe_and_forced_directions();
    test_vlog_auto_probe_reserves_algorithm_budget();
    test_direction_is_physical_and_independent();
    test_monitor_session_preserves_mode();
    test_reference_provisional_blocks_blur();
    test_recalibration_requires_af_evidence();
    test_lighting_change_suppresses_defocus_decision();
    test_blur_confirmation_is_consecutive();
    test_low_texture_does_not_touch_reference();
    test_low_confidence_breaks_blur_consecutiveness();
    test_single_lighting_oscillation_preserves_reference();
    test_post_move_lighting_is_observable();
    test_initial_acquisition_policy();
    test_environmental_abort_is_not_af_failure();
    test_reference_qualification_criteria();
    test_manual_provisional_probe_is_bounded();

    smoothaf_vlog_af_init(&af);
    CHECK(af.mode == SMOOTHAF_VLOG_AF_OFF && !af.armed,
          "Vlog AF initializes OFF and disarmed");
    CHECK(smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 0),
          "monitor task itself does not block manual arming");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_DRY_RUN, 0, 0, 0, 0),
          "arming needs an enabled monitor");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_ACTIVE, 1, 1, 0, 0),
          "another focus task blocks arming");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_OFF, 1, 0, 0, 0),
          "OFF mode cannot be armed");
    CHECK(!smoothaf_vlog_af_can_arm(SMOOTHAF_VLOG_AF_DRY_RUN, 1, 1, 1, 1),
          "an already armed episode blocks a second arming");
    CHECK(smoothaf_vlog_af_command_block_reason(&af, 1, 0) ==
          SMOOTHAF_VLOG_AF_BLOCK_DISARMED &&
          smoothaf_vlog_af_command_block_reason_for(&af, SMOOTHAF_VLOG_AF_OFF, 1, 0) ==
          SMOOTHAF_VLOG_AF_BLOCK_DISARMED,
          "command diagnostics identify a disarmed episode");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    while (af.commands < SMOOTHAF_VLOG_AF_MAX_COMMANDS)
    {
        CHECK(smoothaf_vlog_af_can_command(&af, 1, af.commands + 100),
              "four-command episode accepts commands within its budget");
        smoothaf_vlog_af_record_command(&af, af.commands + 100);
        af.awaiting_result = 0;
        af.state = SMOOTHAF_VLOG_AF_PROBING;
    }
    CHECK(smoothaf_vlog_af_commands_available(&af) == 0,
          "no commands remain after the episode limit");
    CHECK(smoothaf_vlog_af_command_block_reason(&af, 1, 1000) ==
          SMOOTHAF_VLOG_AF_BLOCK_EPISODE_LIMIT,
          "command diagnostics identify the episode limit");
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(af.commands == 0 && af.session_commands == SMOOTHAF_VLOG_AF_MAX_COMMANDS,
          "new episode resets only its counter, not the session budget");
    CHECK(smoothaf_vlog_af_commands_available(&af) == 0 &&
          smoothaf_vlog_af_command_block_reason(&af, 1, 1000) ==
          SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT,
          "second episode reports the global session limit explicitly");
    CHECK(strcmp(smoothaf_vlog_af_block_name(SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT),
                 "SESSION_LIMIT") == 0,
          "command-block reason has a diagnostic name");
    smoothaf_vlog_af_init(&af);
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    CHECK(smoothaf_vlog_af_commands_available(&af) == SMOOTHAF_VLOG_AF_MAX_COMMANDS &&
          smoothaf_vlog_af_can_command(&af, 1, 1000),
          "restarting the monitor is the explicit session-budget recovery");

    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_ACTIVE, 1, 1000);
    af.state = SMOOTHAF_VLOG_AF_PROBING;
    af.awaiting_result = 1;
    CHECK(smoothaf_vlog_af_command_block_reason(&af, 1, 1000) ==
          SMOOTHAF_VLOG_AF_BLOCK_WAITING_RESULT,
          "command diagnostics identify a pending post-move result");
    af.awaiting_result = 0;
    CHECK(smoothaf_vlog_af_command_block_reason_for(&af,
          SMOOTHAF_VLOG_AF_ACTIVE, 0, 1000) ==
          SMOOTHAF_VLOG_AF_BLOCK_DISABLED,
          "command diagnostics identify the ACTIVE Enable gate");
    af.cooldown_until_ms = 2000;
    CHECK(smoothaf_vlog_af_command_block_reason(&af, 1, 1000) ==
          SMOOTHAF_VLOG_AF_BLOCK_COOLDOWN,
          "command diagnostics identify cooldown");

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
    CHECK(m.have_reference && m.reference == 1000 &&
          m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL,
          "first confident sample creates a provisional reference, not focus evidence");
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
    CHECK(m.state != SMOOTHAF_MONITOR_POSSIBLE_BLUR &&
          m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL,
          "a provisional reference never declares blur on its own");
    smoothaf_monitor_qualify_reference(&m, 1000, 120,
                                       SMOOTHAF_MONITOR_METRIC_SAD, 115);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID,
          "only bounded AF evidence promotes the reference to VALID");
    s = sample(100, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 1600);
    smoothaf_monitor_accept(&m, &s, 2100);
    smoothaf_monitor_accept(&m, &s, 2600);
    CHECK(m.state == SMOOTHAF_MONITOR_POSSIBLE_BLUR,
          "persistent loss enters possible blur after qualification");
    CHECK(m.reference == 1000, "blur does not lower reference");
    smoothaf_vlog_af_new_episode(&af, SMOOTHAF_VLOG_AF_DRY_RUN, 1, m.score);
    CHECK(smoothaf_vlog_af_should_probe(&af, m.state, SMOOTHAF_VLOG_AF_DRY_RUN),
          "episode arms from the monitor's real blur state");

    s = sample(1000, 120, 1, 1);
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
    smoothaf_monitor_accept(&m, &s, 8100);
    CHECK(m.state == SMOOTHAF_MONITOR_UNCERTAIN &&
          m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.lighting_count == 1,
          "one lighting excursion pauses decisions without invalidating the reference");
    smoothaf_monitor_accept(&m, &s, 8200);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_OBSOLETE &&
          m.lighting_count == 2,
          "a confirmed lighting change marks the reference obsolete");
    smoothaf_monitor_accept(&m, &s, 8300);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_RECALIBRATE &&
          !m.have_reference && !m.possible_blur && m.lighting_count == 0,
          "persistent environment change enters safe recalibration without declaring focus");
    s = sample(900, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 8400);
    CHECK(m.have_reference && m.reference == 900 &&
          m.reference_state == SMOOTHAF_MONITOR_REF_PROVISIONAL,
          "a new scene starts provisional instead of declaring a focused reference");
    smoothaf_monitor_qualify_reference(&m, 900, 200,
                                       SMOOTHAF_MONITOR_METRIC_SAD, 8450);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID,
          "AF evidence qualifies the recalibrated reference");

    /* A separated excursion must not inherit the previous lighting count and
     * a single oscillation must not force another recalibration. */
    s = sample(1000, 120, 1, 1);
    smoothaf_monitor_accept(&m, &s, 8500);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.lighting_count == 1,
          "a separated lighting excursion starts a fresh consecutive count");
    s = sample(900, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 8600);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.have_reference && m.lighting_count == 0,
          "returning from an excursion preserves the reference without churn");
    s = sample(900, 200, 1, 1);
    smoothaf_monitor_accept(&m, &s, 8700);
    CHECK(m.reference_state == SMOOTHAF_MONITOR_REF_VALID &&
          m.have_reference,
          "the qualified reference remains usable after flicker");

    s = sample(-1, 0, 0, 0);
    smoothaf_monitor_accept(&m, &s, 8800);
    CHECK(m.metrics.invalid == 1, "invalid sample counted separately");
    CHECK(m.reference == 900, "invalid sample does not alter reference");

    smoothaf_monitor_stop(&m);
    CHECK(!m.enabled && m.state == SMOOTHAF_MONITOR_IDLE, "stop freezes session");

    if (failures) {
        printf("%d monitor test(s) FAILED\n", failures);
        return 1;
    }
    printf("SmoothAF monitor tests passed\n");
    return 0;
}
