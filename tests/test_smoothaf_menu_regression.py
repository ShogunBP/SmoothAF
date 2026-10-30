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

    assert menu.count(".name =") == 12
    assert "MENU_EOL" in menu


if __name__ == "__main__":
    test_top_level_menu_does_not_count_menu_eol()
    test_all_smoothaf_menu_entries_have_names()
    print("SmoothAF menu regression tests passed")
