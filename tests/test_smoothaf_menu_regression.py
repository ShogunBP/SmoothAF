import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "smoothaf.c"
EPISODE_SOURCE = ROOT / "src" / "smoothaf_episode.h"


def test_menu_shape_and_lifecycle():
    source = SOURCE.read_text(encoding="utf-8")
    episode_source = EPISODE_SOURCE.read_text(encoding="utf-8")
    implementation = source + "\n" + episode_source
    menu = source.split("static struct menu_entry smoothaf_menu[] =", 1)[1]
    menu = menu.split("static unsigned int smoothaf_init", 1)[0]
    assert "MENU_EOL" in menu
    top_names = [
        "Enable", "Vlog Monitor", "Diagnostic", "AF Performance",
        "Monitor Performance", "Algorithm Performance", "Focus Test",
        "Status", "STOP", "PANIC / Emergency Stop", "Rearm Safe Mode",
        "Build Info",
    ]
    top_positions = [menu.index('{ .name = "' + name + '"') for name in top_names]
    assert top_positions == sorted(top_positions)
    assert top_names[0] == "Enable"
    assert top_names[-4:] == ["STOP", "PANIC / Emergency Stop", "Rearm Safe Mode", "Build Info"]

    vlog = menu.split('{ .name = "Vlog Monitor"', 1)[1].split("MENU_EOL", 1)[0]
    assert '"Sample Int"' in vlog
    assert '"Vlog Metric"' in vlog
    assert '"Vlog Step"' in vlog
    assert '"AF Algorithm"' in vlog
    assert '"AF Direction"' in vlog
    assert 'CHOICES("AUTO", "A", "B")' in vlog
    assert '"Movie Active Exp"' in vlog
    assert '"Movie Active Experimental"' not in vlog
    assert 'static CONFIG_INT("smoothaf.movie_active_experimental"' in source
    assert '"Far Focus Recovery"' in vlog
    assert '.priv = &smoothaf_far_focus_recovery' in vlog
    assert 'MENU_SET_VALUE("%s", smoothaf_far_focus_recovery ? "ON" : "OFF")' in source
    assert '"Monitoring"' in vlog
    assert '"Start/Stop Monitoring"' not in vlog
    assert vlog.index('"Monitoring"') < vlog.index('"Start New AF Episode"')
    assert vlog.index('"Start New AF Episode"') < vlog.index('"Vlog AF Mode"')
    assert 'smoothaf_monitor_toggle_update' in source
    assert 'smoothaf_monitor_task_state_name_for(' in source
    assert vlog.index('"Sample Int"') < vlog.index('"Session State"')
    assert vlog.index('"Session State"') < vlog.index('"AF State"') < vlog.index('"Monitor Status"')
    assert '"STOP"' not in vlog
    assert '"PANIC / Emergency Stop"' not in vlog
    assert '"Rearm Safe Mode"' not in vlog
    assert '"Sampling Interval"' not in menu

    focus_test = menu.split('{ .name = "Focus Test"', 1)[1].split("MENU_EOL", 1)[0]
    for name in (
        '"Start Focus Test"', '"AF Algorithm"', '"Test Metric"', '"Test Step"',
        '"Test Direction"', '"AUTO"', '"A"', '"B"', '"Test Status"',
        '"Test Result"', '"AUTO Result"', '"AUTO CMDs"', '"AUTO Scores"',
        '"AUTO Deltas"', '"Test Duration"',
    ):
        assert name in focus_test
    assert '"AF Mode"' not in focus_test
    assert '"Sharpness Metric"' not in focus_test
    assert '"Step Size"' not in focus_test

    for name in (
        '"AF CMDs This EP"', '"AUTO Probe CMDs"', '"Algorithm CMDs"',
        '"Next EP Block"', '"AF CD Remain"',
        '"AF Session"', '"EP Result"', '"AF EPs Done"', '"AF Armed"',
        '"Blur Recheck"', '"AF Last Dir"', '"SAD Score"', '"Lap Score"',
        '"Actual Int"', '"REC State"', '"Algorithm"', '"Search Dir"',
        '"Converge Why"', '"EP Summary"', '"Log Page"', '"Log Entry"',
        '"Focus Step"', '"STOP"', '"PANIC / Emergency Stop"',
    ):
        assert name in menu

    assert 'menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    assert 'menu_remove("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    for forbidden in (
        '"Canon AF Assist"', '"Trigger Canon AF"', '"lens_autofocus"',
        '"PTP_AF_START"', '"lens_setup_af"', '"module_send_keypress"',
        '"Export AF Log"', '"Test SD Write"', '"Test Root Create"',
        '"Test Root Edit"', '"Test File Paths"', '"Path Entry"',
        '"Path Result"', 'FIO_CreateFile(', 'FIO_WriteFile(', 'FIO_CloseFile(',
    ):
        assert forbidden not in source
    assert 'smoothaf_af_assist' not in source
    assert 'SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS' not in source
    for logger_menu_name in (
        '"Persistent Logger"', '"Logger FIO Enable"',
        '"Logger Max File KB"', '"Logger Max Files"',
        '"Logger Fill Level"', '"Logger Dropped"',
    ):
        assert logger_menu_name in menu
    assert 'smoothaf_log_' in source
    assert 'smoothaf_diag_' in source
    assert '#include "smoothaf_diagnostics.h"' in source
    assert 'smoothaf_telemetry' in source
    assert 'smoothaf_monitor_task' in source
    assert 'smoothaf_task_stop_requested' in source
    assert 'smoothaf_task_exited = 1;' in source
    assert 'lens_focus(direction, step, 1, 0)' in source
    assert 'smoothaf_panic_latch' in source
    assert 'smoothaf_panic_rearm' in source
    assert 'smoothaf_can_start(smoothaf_enabled' in source
    assert 'smoothaf_vlog_af_can_arm(' in source
    assert 'smoothaf_vlog_af_command_block_reason' in source
    assert 'smoothaf_vlog_af_commands_available' in source
    assert 'SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT' in source
    assert 'smoothaf_vlog_af_should_probe_reference(' in source
    assert 'smoothaf_vlog_af_should_acquire_continuous(' in source
    assert 'SMOOTHAF_MONITOR_REF_PROVISIONAL' in source
    assert 'smoothaf_vlog_af_advance_cooldown(' in source
    assert 'smoothaf_vlog_af_timeout_due(' in source
    assert 'smoothaf_vlog_af_should_cancel(' in source
    assert 'smoothaf_vlog_af_uses_dry_run(' in source
    assert 'smoothaf_vlog_af.mode != SMOOTHAF_VLOG_AF_DRY_RUN' in source
    assert 'smoothaf_movie_dry_run_recording_allowed(' in source
    assert 'SMOOTHAF_LOG_RECORDING' in source
    assert 'smoothaf_movie_recording_transition(' in source
    assert 'smoothaf_movie_recording_cbr' in source
    assert 'smoothaf_movie_start_automatic' in source
    assert 'smoothaf_movie_stop_automatic' in source
    assert 'smoothaf_vlog_af_reset_session' in source
    assert 'SMOOTHAF_BUILD_ID' in source
    assert 'smoothaf_movie_automatic_session' in source
    assert 'smoothaf_movie_start_pending' in source
    assert 'smoothaf_movie_active_experimental = 0;' in source
    assert 'smoothaf_build_info_update' in source
    assert 'smoothaf_cancel_pending_measure(' in source
    assert 'smoothaf_measure_route_for(' in source
    assert 'smoothaf_diag_page_after_refresh(' in source
    assert 'smoothaf_log_total(' in source
    assert 'SMOOTHAF_LOG_MEASURE' in source
    assert 'SMOOTHAF_LOG_CONTROLLER' in source
    assert 'SMOOTHAF_LOG_ERROR' in source
    assert 'SMOOTHAF_LOG_ABORT' in source
    assert source.count('MENU_SET_HELP(') >= 8
    assert 'smoothaf_af_direction_value()' in source
    assert 'smoothaf_vlog_direction_for_episode()' in source
    assert 'smoothaf_direction_value()' in source
    assert source.count('smoothaf_af_direction_value()') >= 1
    assert 'smoothaf_vlog_direction_migrate(' in source
    assert 'SMOOTHAF_VLOG_DIRECTION_AUTO' in source
    assert 'static CONFIG_INT("smoothaf.vlog_direction", smoothaf_vlog_direction, -1);' in source
    assert 'smoothaf_vlog_af_auto_needs_move(' in implementation
    assert 'smoothaf_vlog_af_auto_on_score(' in implementation
    assert 'smoothaf_vlog_af_set_episode_limit(&smoothaf_vlog_af' in source
    assert 'smoothaf_vlog_af_session_command_limit_for(' in source
    assert 'config_var_was_changed(&smoothaf_step)' in source
    assert 'smoothaf_test_step = config_var_was_changed(&smoothaf_step)' in source
    assert 'smoothaf_vlog_step = config_var_was_changed(&smoothaf_step)' in source
    assert 'MENU_SET_VALUE("%s", smoothaf_far_focus_recovery ? "ON" : "OFF")' in source
    assert 'MENU_SET_HELP("phase=%s result=%s cmd=%d best=%d pos=%d moves=%d"' in source
    assert 'smoothaf_measure_with_metric(smoothaf_test_metric' in source
    assert 'smoothaf_measure_with_metric(smoothaf_vlog_metric' in source
    assert 'smoothaf_move_with_step(dir, smoothaf_test_step)' in source
    assert 'smoothaf_move_with_step(direction, smoothaf_vlog_step)' in source
    assert 'MODULE_CONFIG(smoothaf_test_metric)' in source
    assert 'MODULE_CONFIG(smoothaf_vlog_metric)' in source
    assert 'MODULE_CONFIG(smoothaf_test_step)' in source
    assert 'MODULE_CONFIG(smoothaf_vlog_step)' in source
    assert 'MODULE_CONFIG(smoothaf_test_direction)' in source
    assert re.search(r"struct smoothaf_telemetry\s*\{", (ROOT / "src" / "smoothaf_diagnostics.h").read_text())


def test_removed_integration_files_are_not_present():
    for relative in ("src/smoothaf_af_assist.h", "src/smoothaf_log_export.h", "src/smoothaf_fio_test.h"):
        assert not (ROOT / relative).exists(), relative


def test_production_runtime_diagnostics_invariants():
    source = SOURCE.read_text(encoding="utf-8")
    lock = source.split("static int smoothaf_lock(void)", 1)[1].split(
        "static void smoothaf_unlock(void)", 1
    )[0]
    note_error = source.split("static void smoothaf_runtime_note_error", 1)[1].split(
        "static void smoothaf_runtime_increment", 1
    )[0]

    # Every production semaphore operation must remain behind the instrumented
    # wrappers, including the no-handle failure path.
    assert source.count("take_semaphore(") == 1
    assert source.count("give_semaphore(") == 1
    assert "if (!smoothaf_sem)" in lock
    assert "smoothaf_runtime_increment(&smoothaf_runtime_take_count);" in lock
    assert "smoothaf_runtime_increment(&smoothaf_runtime_take_failures);" in lock
    assert "smoothaf_runtime_last_take_caller = (uintptr_t)current_task;" in lock
    assert "smoothaf_runtime_last_take_duration = 0;" in lock
    assert "SMOOTHAF_RUNTIME_RESULT_NO_HANDLE" in lock
    assert "smoothaf_runtime_note_error(SMOOTHAF_RUNTIME_ERROR_SEM_TAKE" in lock

    # Initialization publishes the semaphore result before READY; a later
    # change must not make an unverified handle look ready.
    init = source.split("static unsigned int smoothaf_init(void)", 1)[1]
    create_publish = init.index("smoothaf_runtime_sem_create_result = smoothaf_sem ? 1 : 0;")
    ready_guard = init.index("if (smoothaf_runtime_sem_create_result != 1 || !smoothaf_runtime_sem_handle)")
    ready_publish = init.index("smoothaf_runtime_stage = SMOOTHAF_RUNTIME_STAGE_READY;")
    assert create_publish < ready_guard < ready_publish
    assert "smoothaf_runtime_note_error(SMOOTHAF_RUNTIME_ERROR_READY" in init
    assert "READY INVARIANT" in source

    # The first runtime error is a latch: menu refreshes and later errors must
    # not clear or replace it.
    assert "if (!smoothaf_runtime_first_error)" in note_error
    assert "smoothaf_runtime_first_error = error;" in note_error
    assert "smoothaf_runtime_last_error = error;" in note_error

    # Canon's Legacy formatter path has demonstrated compatibility with %d,
    # not the newly introduced %u. Keep runtime counters/durations on %d.
    assert 'MENU_SET_VALUE("T=%d F=%d"' in source
    assert 'MENU_SET_VALUE("G=%d F=%d"' in source
    assert 'MENU_SET_VALUE("T=%u F=%u"' not in source
    assert 'MENU_SET_VALUE("G=%u F=%u"' not in source


def test_production_menu_and_task_start_paths():
    source = SOURCE.read_text(encoding="utf-8")
    logger_select = source.split(
        "static MENU_SELECT_FUNC(smoothaf_persist_logger_config_select)", 1
    )[1].split("static MENU_SELECT_FUNC(smoothaf_measure_select)", 1)[0]
    monitor_select = source.split(
        "static MENU_SELECT_FUNC(smoothaf_monitor_toggle_select)", 1
    )[1].split("static const char *smoothaf_monitor_task_state_name", 1)[0]
    movie_start = source.split(
        "static void smoothaf_movie_start_automatic(void)", 1
    )[1].split("static void smoothaf_movie_stop_automatic(void)\n{", 1)[0]
    start_task = source.split(
        "static int smoothaf_start_task(int task_id, int requires_enable)\n{", 1
    )[1].split("/* A queued Measure", 1)[0]

    # Legacy calls a custom .select instead of applying the default .priv
    # toggle, so this production callback must perform the toggle explicitly.
    assert "smoothaf_logger_config_toggle(priv, delta, menu_numeric_toggle)" in logger_select
    assert '#include "smoothaf_menu_toggle.h"' in source
    assert logger_select.index("smoothaf_logger_config_toggle") < logger_select.index(
        "smoothaf_persist_logger_sync_config();"
    )
    assert source.count(".select = smoothaf_persist_logger_config_select") == 2
    assert source.count(".priv = &smoothaf_logger_enabled") == 1
    assert source.count(".priv = &smoothaf_logger_fio_enabled") == 1

    # Monitoring and Movie Active Exp leave task_exited to the common starter;
    # a failed semaphore acquisition therefore cannot leave a false 'active'
    # state behind before a task exists.
    assert "smoothaf_task_exited = 0;" not in monitor_select
    assert "smoothaf_task_exited = 0;" not in movie_start
    assert "if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))" in monitor_select
    assert "if (!smoothaf_start_task(SMOOTHAF_TASK_MONITOR, 0))" in movie_start
    assert '#include "smoothaf_task_lifecycle.h"' in source
    assert "if (!smoothaf_lock())" in start_task
    lock_failure = start_task.split("if (!smoothaf_lock())", 1)[1].split(
        "    if ((", 1
    )[0]
    assert "smoothaf_task_running = 1;" not in lock_failure
    assert "smoothaf_task_exited = 0;" not in lock_failure
    assert "return 0;" in lock_failure
    assert "smoothaf_task_create_failed(&smoothaf_task_running" in start_task
    assert "smoothaf_task_running = 0;" not in start_task
    assert "smoothaf_task_exited = 1;" not in start_task
    assert start_task.index("smoothaf_task_reserve(") < start_task.index(
        "task = task_create("
    )
    assert start_task.index("task = task_create(") < start_task.index(
        "smoothaf_task_create_failed("
    )


if __name__ == "__main__":
    test_production_runtime_diagnostics_invariants()
    test_production_menu_and_task_start_paths()

    test_menu_shape_and_lifecycle()
    test_removed_integration_files_are_not_present()
    print("SmoothAF menu regression tests passed")
