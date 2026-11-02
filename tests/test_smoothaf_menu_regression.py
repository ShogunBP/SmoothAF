import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "smoothaf.c"


def test_menu_shape_and_lifecycle():
    source = SOURCE.read_text(encoding="utf-8")
    menu = source.split("static struct menu_entry smoothaf_menu[] =", 1)[1]
    menu = menu.split("/* ---------------------------------------------------------------- */", 1)[0]
    assert "MENU_EOL" in menu
    assert 'menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    assert 'menu_remove("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));' in source
    for name in (
        '"Vlog Monitor"', '"Start/Stop Monitoring"', '"Monitor Performance"',
        '"SAD Score"', '"Laplacian Score"', '"Sampling Interval"',
        '"Measure"', '"Focus Step"', '"Focus Test"', '"AF Mode"',
        '"Sharpness Metric"', '"Step Size"', '"Status"', '"Stop"',
        '"PANIC / Emergency Stop"', '"Rearm Safe Mode"'):
        assert name in menu
    for forbidden in (
        '"Canon AF Assist"', '"Trigger Canon AF"', '"Debug Logging"',
        '"Log Count"', '"Log Failures"', '"Log Page"', '"Log Entry"',
        '"Previous"', '"Next"', '"Clear History"', '"Export AF Log"',
        '"Test SD Write"', '"Test Root Create"', '"Test Root Edit"',
        '"Test File Paths"', '"Path Entry"', '"Path Result"', 'FIO_'):
        assert forbidden not in source
    assert 'smoothaf_af_assist' not in source
    assert 'SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS' not in source
    assert 'smoothaf_log_' not in source
    assert 'smoothaf_diag_' not in source
    assert '#include "smoothaf_diagnostics.h"' in source
    assert 'smoothaf_telemetry' in source
    assert 'smoothaf_monitor_task' in source
    assert 'smoothaf_task_stop_requested' in source
    assert 'smoothaf_task_exited = 1;' in source
    assert 'lens_focus(direction, smoothaf_step, 1, 0)' in source
    assert 'smoothaf_panic_latch' in source
    assert 'smoothaf_panic_rearm' in source
    assert 'smoothaf_can_start(smoothaf_enabled' in source
    assert re.search(r"struct smoothaf_telemetry\s*\{", (ROOT / "src" / "smoothaf_diagnostics.h").read_text())


def test_removed_integration_files_are_not_present():
    for relative in ("src/smoothaf_af_assist.h", "src/smoothaf_log_export.h", "src/smoothaf_fio_test.h"):
        assert not (ROOT / relative).exists(), relative


if __name__ == "__main__":
    test_menu_shape_and_lifecycle()
    test_removed_integration_files_are_not_present()
    print("SmoothAF menu regression tests passed")
