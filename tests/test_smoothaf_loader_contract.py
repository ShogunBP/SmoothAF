from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "smoothaf.c").read_text(encoding="utf-8")
LOADER = (ROOT / "docs" / "references" / "magic-lantern-branch-lua_fix" / "src" / "module.c").read_text(encoding="utf-8")


def test_legacy_init_contract_and_failed_module_lifecycle():
    init_call = LOADER.index("int err = module_list[mod].info->init();")
    failure_branch = LOADER.index("if (err)", init_call)
    disable_callbacks = LOADER.index("module_list[mod].cbr = 0;", failure_branch)
    registration = LOADER.index("if(!module_list[mod].error)", disable_callbacks)
    unload_filter = LOADER.index("!module_list[mod].error")
    deinit = LOADER.index("module_list[mod].info->deinit();")

    assert "if (err)" in LOADER[init_call:failure_branch + 30]
    assert "module_list[mod].error = err;" in LOADER[failure_branch:disable_callbacks]
    assert disable_callbacks < registration
    unload_filter = LOADER.index("if(module_list[mod].valid && module_list[mod].enabled && !module_list[mod].error)", deinit)
    assert unload_filter < deinit
    assert "module_list[mod].valid = 0;" in LOADER[deinit:deinit + 180]


def test_failed_init_publishes_only_read_only_runtime_menu():
    init = SOURCE.split("static unsigned int smoothaf_init(void)", 1)[1]
    runtime_add = init.index('menu_add("SmoothAF Runtime", smoothaf_runtime_root_menu, COUNT(smoothaf_runtime_root_menu));')
    create = init.index('smoothaf_sem = create_named_semaphore("smoothaf_sem", 1);')
    fail_return = init.index("if (!smoothaf_sem)")
    fail_return = init.index("return 1;", fail_return)
    operational_add = init.index('menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));')
    assert runtime_add < create < fail_return < operational_add
    runtime_menu = SOURCE.split("static struct menu_entry smoothaf_runtime_menu[] =", 1)[1].split(
        "static struct menu_entry smoothaf_runtime_root_menu[] =", 1
    )[0]
    assert ".select" not in runtime_menu
    assert ".priv" not in runtime_menu
    assert "smoothaf_runtime" in runtime_menu


def test_operational_menu_is_not_registered_before_ready():
    init = SOURCE.split("static unsigned int smoothaf_init(void)", 1)[1]
    ready = init.index("smoothaf_runtime_stage = SMOOTHAF_RUNTIME_STAGE_READY;")
    operational_add = init.index('menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));')
    assert ready < operational_add


def test_failed_init_has_no_production_movement_or_task_path():
    init = SOURCE.split("static unsigned int smoothaf_init(void)", 1)[1]
    failure = init.index("if (!smoothaf_sem)")
    failure_return = init.index("return 1;", failure)
    assert "smoothaf_start_task(" not in init[:failure_return]
    assert "smoothaf_start_fio_task(" not in init[:failure_return]
    assert "lens_focus(" not in init[:failure_return]
