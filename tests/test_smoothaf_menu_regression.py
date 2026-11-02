import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "smoothaf.c"


def test_menu_shape_and_lifecycle():
    source = SOURCE.read_text(encoding="utf-8")
    menu = source.split("static struct menu_entry smoothaf_menu[] =", 1)[1]
    menu = menu.split("static unsigned int smoothaf_init", 1)[0]
    assert "MENU_EOL" in menu
    top_names = [
        "Enable", "Vlog Monitor", "Diagnostic", "AF Performance",
        "Monitor Performance", "Algorithm Performance", "Focus Test",
        "AF Mode", "Sharpness Metric", "Step Size", "Status", "STOP",
        "PANIC / Emergency Stop", "Rearm Safe Mode", "Build Info",
    ]
    top_positions = [menu.index('{ .name = "' + name + '"') for name in top_names]
    assert top_positions == sorted(top_positions)
    assert top_names[0] == "Enable"
    assert top_names[-4:] == ["STOP", "PANIC / Emergency Stop", "Rearm Safe Mode", "Build Info"]

    vlog = menu.split('{ .name = "Vlog Monitor"', 1)[1].split("MENU_EOL", 1)[0]
    assert '"Sample Int"' in vlog
    assert vlog.index('"Start/Stop Monitoring"') < vlog.index('"Start New AF Episode"')
    assert vlog.index('"Start New AF Episode"') < vlog.index('"Vlog AF Mode"')
    assert vlog.index('"Sample Int"') < vlog.index('"Session State"')
    assert vlog.index('"Session State"') < vlog.index('"AF State"') < vlog.index('"Monitor Status"')
    assert '"STOP"' not in vlog
    assert '"PANIC / Emergency Stop"' not in vlog
    assert '"Rearm Safe Mode"' not in vlog
    assert '"Sampling Interval"' not in menu

    for name in (
        '"AF CMDs This EP"', '"Next EP Block"', '"AF CD Remain"',
        '"AF Session"', '"EP Result"', '"AF EPs Done"', '"AF Armed"',
        '"Blur Recheck"', '"AF Last Dir"', '"SAD Score"', '"Lap Score"', '"Actual Int"', '"REC State"',
        '"Algorithm"', '"Search Dir"', '"Converge Why"', '"EP Summary"',
        '"Log Page"', '"Log Entry"', '"Focus Step"', '"STOP"', '"PANIC / Emergency Stop"',
    ):
        assert name in menu

    assert 'menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    assert 'menu_remove("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    for forbidden in (
        '"Canon AF Assist"', '"Trigger Canon AF"', '"lens_autofocus"',
        '"PTP_AF_START"', '"lens_setup_af"', '"module_send_keypress"',
        '"Export AF Log"', '"Test SD Write"', '"Test Root Create"',
        '"Test Root Edit"', '"Test File Paths"', '"Path Entry"',
        '"Path Result"', 'FIO_',
    ):
        assert forbidden not in source
    assert 'smoothaf_af_assist' not in source
    assert 'SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS' not in source
    assert 'smoothaf_log_' in source
    assert 'smoothaf_diag_' in source
    assert '#include "smoothaf_diagnostics.h"' in source
    assert 'smoothaf_telemetry' in source
    assert 'smoothaf_monitor_task' in source
    assert 'smoothaf_task_stop_requested' in source
    assert 'smoothaf_task_exited = 1;' in source
    assert 'lens_focus(direction, smoothaf_step, 1, 0)' in source
    assert 'smoothaf_panic_latch' in source
    assert 'smoothaf_panic_rearm' in source
    assert 'smoothaf_can_start(smoothaf_enabled' in source
    assert 'smoothaf_vlog_af_can_arm(' in source
    assert 'smoothaf_vlog_af_command_block_reason' in source
    assert 'smoothaf_vlog_af_commands_available' in source
    assert 'SMOOTHAF_VLOG_AF_BLOCK_SESSION_LIMIT' in source
    assert 'smoothaf_vlog_af_should_probe(' in source
    assert 'smoothaf_vlog_af_advance_cooldown(' in source
    assert 'smoothaf_vlog_af_timeout_due(' in source
    assert 'smoothaf_vlog_af_should_cancel(' in source
    assert 'smoothaf_vlog_af_uses_dry_run(' in source
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
    assert 'smoothaf_direction_value()' in source
    assert source.count('smoothaf_af_direction_value()') >= 4
    assert re.search(r"struct smoothaf_telemetry\s*\{", (ROOT / "src" / "smoothaf_diagnostics.h").read_text())


def test_removed_integration_files_are_not_present():
    for relative in ("src/smoothaf_af_assist.h", "src/smoothaf_log_export.h", "src/smoothaf_fio_test.h"):
        assert not (ROOT / relative).exists(), relative


if __name__ == "__main__":
    test_menu_shape_and_lifecycle()
    test_removed_integration_files_are_not_present()
    print("SmoothAF menu regression tests passed")
