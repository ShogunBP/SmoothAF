from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "smoothaf.c"


def test_top_level_menu_does_not_count_menu_eol():
    source = SOURCE.read_text(encoding="utf-8")
    menu = source.split("static struct menu_entry smoothaf_menu[] =", 1)[1]
    menu = menu.split("/* ---------------------------------------------------------------- */", 1)[0]

    assert "MENU_EOL,\n};" not in menu
    assert "MENU_EOL,\n        }," in menu
    assert "menu_add(\"SmoothAF\", smoothaf_menu, COUNT(smoothaf_menu));" in source
    assert "menu_remove(\"SmoothAF\", smoothaf_menu, COUNT(smoothaf_menu));" in source


def test_all_smoothaf_menu_entries_have_names():
    source = SOURCE.read_text(encoding="utf-8")
    menu = source.split("static struct menu_entry smoothaf_menu[] =", 1)[1]
    menu = menu.split("/* ---------------------------------------------------------------- */", 1)[0]

    assert menu.count(".name =") == 32
    assert '"Vlog Monitor"' in menu
    assert '"Start/Stop Monitoring"' in menu
    assert '"Monitor Status"' in menu
    assert '"SAD Score"' in menu
    assert '"Laplacian Score"' in menu
    assert '"Smoothed Score"' in menu
    assert '"Reference Score"' in menu
    assert '"Possible Blur"' in menu
    assert '"Sampling Interval"' in menu
    assert '"Sample Count"' in menu
    assert '"Log Count"' in menu
    assert '"Log Failures"' in menu
    assert '"Canon AF Assist"' in menu
    assert '"Trigger Canon AF"' in menu
    assert 'SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED' in source
    assert 'smoothaf_af_assist_block_name' in source
    assert 'smoothaf_af_assist_request' in source
    assert '"AF Mode"' in menu
    assert '"Debug Logging"' in menu
    assert 'CHOICES("Legacy HC", "Adaptive HC", "Normal AF")' in menu
    assert 'CONFIG_INT("smoothaf.af_mode", smoothaf_af_mode, -1)' in source
    assert "smoothaf_normal_run" in source
    assert "smoothaf_adaptive_run" in source
    assert "lens_focus(direction, smoothaf_step, 1, 0)" in source
    assert "MENU_EOL" in menu
    assert '"Log Page"' in menu
    assert '"Log Entry"' in menu
    assert '"Previous"' in menu
    assert '"Next"' in menu
    assert '"Clear History"' in menu
    assert '"PANIC / Emergency Stop"' not in menu
    assert '"Rearm Safe Mode"' not in menu
    assert 'smoothaf_panic' not in source
    assert 'Safe Mode' not in source
    assert 'Rearm' not in source
    assert 'task_create' not in menu.split('"Vlog Monitor"', 1)[1].split('"Diagnostic"', 1)[0]
    assert 'lens_focus' not in menu.split('"Vlog Monitor"', 1)[1].split('"Diagnostic"', 1)[0]
    assert 'get_yuv422_vram' not in menu.split('"Vlog Monitor"', 1)[1].split('"Diagnostic"', 1)[0]
    assert "smoothaf_diag_previous_select" in source
    assert "smoothaf_diag_next_select" in source
    assert "smoothaf_log_clear(&smoothaf_log)" in source
    assert "smoothaf_start_task" not in menu.split('"Log Page"', 1)[1].split("MENU_EOL", 1)[0]


if __name__ == "__main__":
    test_top_level_menu_does_not_count_menu_eol()
    test_all_smoothaf_menu_entries_have_names()
    print("SmoothAF menu regression tests passed")
