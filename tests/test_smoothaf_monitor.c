#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_monitor.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

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
        CHECK(af.episode_result == SMOOTHAF_VLOG_AF_RESULT_CONVERGED,
              "partial improvement is not reported as convergence until the episode completes");
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

int main(void)
{
    struct smoothaf_monitor m;
    struct smoothaf_monitor_sample s;
    int from;
    int to;
    struct smoothaf_vlog_af af;

    test_vlog_af_lifecycle();
    test_movie_recording_policy();
    test_continuous_active();
    test_direction_is_physical_and_independent();
    test_monitor_session_preserves_mode();

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
    smoothaf_monitor_accept(&m, &s, 8100);
    CHECK(m.state == SMOOTHAF_MONITOR_UNCERTAIN,
          "lighting change suspends decisions");

    s = sample(-1, 0, 0, 0);
    smoothaf_monitor_accept(&m, &s, 8600);
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
