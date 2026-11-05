from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "smoothaf.c").read_text(encoding="utf-8")
LOADER = (ROOT / "docs" / "references" / "magic-lantern-branch-lua_fix" / "src" / "module.c").read_text(encoding="utf-8")
MENU = (ROOT / "docs" / "references" / "magic-lantern-branch-lua_fix" / "src" / "menu.c").read_text(encoding="utf-8")


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


def test_legacy_module_identity_and_reload_contract():
    assert "if (module_code)" in LOADER
    assert "if (module_state)" in LOADER
    assert "printf(\"Modules already loaded.\\n\")" in LOADER
    assert "tcc_relocate(state, NULL)" in LOADER
    assert "module_code = buf;" in LOADER
    assert "tcc_delete(state);" in LOADER
    assert "module_list[module_cnt].filename" in LOADER
    assert "module_list[module_cnt].long_filename" in LOADER
    assert "module_list[mod].info->init()" in LOADER
    assert "module_list[mod].cbr = 0;" in LOADER
    assert "module_list[mod].prop_handlers = 0;" in LOADER
    assert "module_list[mod].valid = 0;" in LOADER
    assert "module_exec_cbr" in LOADER
    assert LOADER.count("if(module_list[mod].valid && cbr)") >= 1
    # The source supports one active loader state, but static inspection cannot
    # prove that a previously registered menu callback cannot race a redraw.


def test_legacy_menu_add_remove_contract_is_flat_safe():
    add_internal = MENU.index("menu_add_internal(")
    add_lookup = MENU.index("menu_find_by_name_internal( name, 0)", add_internal)
    remove = MENU.index("void\nmenu_remove(")
    remove_lookup = MENU.index("menu_find_by_name( name, 0)", remove)
    remove_entry = MENU.index("static void menu_remove_entry", remove)
    submenu_cleanup = MENU.index("if (entry->children)", remove_entry)
    assert add_internal < add_lookup
    assert remove < remove_lookup < remove_entry < submenu_cleanup
    assert "menu_remove_entry(menu, entry);" in MENU[remove:remove + 1000]
    assert "menu_add_internal(entry->name, entry->children, count);" in MENU[add_internal:add_internal + 1700]


def test_failed_init_publishes_only_read_only_runtime_menu():
    init = SOURCE.rsplit("static unsigned int smoothaf_init(void)", 1)[1]
    runtime_add = init.index('menu_add("SmoothAF Runtime", smoothaf_runtime_init_menu, COUNT(smoothaf_runtime_init_menu));')
    create = init.index('smoothaf_sem = create_named_semaphore("smoothaf_sem", 1);')
    fail_return = init.index("if (!smoothaf_sem)")
    fail_return = init.index("return 1;", fail_return)
    operational_add = init.index('menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));')
    assert runtime_add < create < fail_return < operational_add
    runtime_menu = SOURCE.split("static struct menu_entry smoothaf_runtime_init_menu[] =", 1)[1].split(
        "static struct menu_entry smoothaf_menu[] =", 1
    )[0]
    assert ".select" not in runtime_menu
    assert ".priv" not in runtime_menu
    assert ".children" not in runtime_menu
    assert "smoothaf_runtime" in runtime_menu


def test_diagnostic_update_callbacks_are_read_only_and_safe_before_init():
    callback_names = (
        "smoothaf_runtime_stage_update",
        "smoothaf_runtime_init_state_update",
        "smoothaf_runtime_sem_create_update",
        "smoothaf_runtime_sem_handle_update",
        "smoothaf_runtime_sem_current_update",
        "smoothaf_runtime_sem_snapshot_update",
        "smoothaf_runtime_sem_copy_update",
        "smoothaf_runtime_sem_end_update",
        "smoothaf_runtime_take_result_update",
        "smoothaf_runtime_last_take_update",
        "smoothaf_runtime_first_error_update",
        "smoothaf_runtime_error_update",
        "smoothaf_runtime_code_identity_update",
        "smoothaf_runtime_data_identity_update",
        "smoothaf_build_code_identity_update",
        "smoothaf_build_data_identity_update",
    )
    forbidden = (
        "smoothaf_lock(", "take_semaphore(", "give_semaphore(",
        "smoothaf_start_task(", "smoothaf_start_fio_task(",
        "lens_focus(", "FIO_", "config_var", "menu_add(", "menu_remove(",
    )
    for name in callback_names:
        body = SOURCE.split("static MENU_UPDATE_FUNC(" + name + ")", 1)[1].split(
            "static ", 1
        )[0]
        for token in forbidden:
            assert token not in body, (name, token)
        assert "MENU_SET_VALUE(" in body


def test_init_lifecycle_and_snapshot_contracts_are_latched():
    init = SOURCE.rsplit("static unsigned int smoothaf_init(void)", 1)[1]
    assert "smoothaf_runtime_increment(&smoothaf_runtime_init_started);" in init
    assert "smoothaf_runtime_increment(&smoothaf_runtime_init_finished);" in init
    assert "smoothaf_runtime_increment(&smoothaf_runtime_init_duplicates);" in init
    assert SOURCE.count("smoothaf_runtime_increment(&smoothaf_runtime_init_duplicates);") == 1
    assert "SMOOTHAF_INIT_STAGE_DUPLICATE" not in SOURCE
    assert "smoothaf_runtime_init_failure(" in init
    assert "smoothaf_runtime_init_stage = SMOOTHAF_INIT_STAGE_FINISHED;" in init
    assert "smoothaf_runtime_init_lifecycle = SMOOTHAF_INIT_LIFECYCLE_SUCCESS;" in init
    assert "smoothaf_runtime_init_first_error" in SOURCE
    assert "smoothaf_runtime_init_reset_attempt_fields();" in init
    assert "smoothaf_runtime_sem_storage_address = (uintptr_t)&smoothaf_sem;" in SOURCE
    for value in (
        "smoothaf_runtime_sem_created_value",
        "smoothaf_runtime_sem_copied_value",
        "smoothaf_runtime_sem_post_create_value",
        "smoothaf_runtime_sem_end_init_value",
    ):
        assert value in init or value in SOURCE
    assert "smoothaf_runtime_sem_first_no_handle_valid" in SOURCE
    assert "smoothaf_runtime_sem_first_no_handle_value = (uintptr_t)smoothaf_sem;" in SOURCE
    assert "smoothaf_runtime_sem_first_no_handle_valid = 1;" in SOURCE
    assert SOURCE.count("smoothaf_runtime_increment(&smoothaf_runtime_init_duplicates);") == 1
    assert "int duplicate_init = smoothaf_runtime_init_lifecycle != SMOOTHAF_INIT_LIFECYCLE_NONE;" in init
    assert "smoothaf_runtime_init_duplicate_failure();" in init
    assert "smoothaf_runtime_init_first_error = error;" in SOURCE
    assert "smoothaf_runtime_init_failure(SMOOTHAF_RUNTIME_ERROR_INIT_DUPLICATE" in init
    assert "smoothaf_runtime_init_first_error = 0;" not in init
    assert SOURCE.count("(uintptr_t)&smoothaf_init") == 2
    assert SOURCE.count("(uintptr_t)&smoothaf_lock") == 2
    assert SOURCE.count("(uintptr_t)&smoothaf_sem") == 4
    assert SOURCE.count("(uintptr_t)&smoothaf_runtime_sem_create_result") == 2


def test_operational_menu_is_not_registered_before_ready():
    init = SOURCE.rsplit("static unsigned int smoothaf_init(void)", 1)[1]
    ready = init.index("smoothaf_runtime_stage = SMOOTHAF_RUNTIME_STAGE_READY;")
    operational_add = init.index('menu_add("SmoothAF", smoothaf_menu, COUNT(smoothaf_menu));')
    assert ready < operational_add


def test_failed_init_has_no_production_movement_or_task_path():
    init = SOURCE.rsplit("static unsigned int smoothaf_init(void)", 1)[1]
    failure = init.index("if (!smoothaf_sem)")
    failure_return = init.index("return 1;", failure)
    assert "smoothaf_start_task(" not in init[:failure_return]
    assert "smoothaf_start_fio_task(" not in init[:failure_return]
    assert "lens_focus(" not in init[:failure_return]
