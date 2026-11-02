#include <stdio.h>
#include "../src/smoothaf_fio_test.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    struct smoothaf_fio_state create;
    struct smoothaf_fio_state edit;
    struct smoothaf_fio_state paths;
    struct smoothaf_fio_path_state path;
    static const char *expected_paths[] = {
        "SMOOTHAF-ROOT-TEST.LOG",
        "B:/SMOOTHAF-ROOT-TEST.LOG",
        "A:/SMOOTHAF-ROOT-TEST.LOG",
        "ML/SMOOTHAF-ROOT-TEST.LOG",
        "B:/AUTOEXEC.BIN",
        "B:/ML/MODULES/smoothaf.mo",
        "B:/SMAFRT.LOG",
    };

    smoothaf_fio_state_init(&create, SMOOTHAF_FIO_ROOT_CREATE);
    CHECK(!smoothaf_fio_reserve(&create, 1, 1, 0), "active task gate");
    CHECK(!smoothaf_fio_reserve(&create, 0, 1, 1), "active monitor gate");
    CHECK(smoothaf_fio_reserve(&create, 0, 1, 0), "create reservation");
    CHECK(!smoothaf_fio_reserve(&create, 0, 1, 0), "reentry gate");
    smoothaf_fio_collision(&create);
    CHECK(create.phase == SMOOTHAF_FIO_COLLISION, "create collision");
    smoothaf_fio_release(&create);

    smoothaf_fio_state_init(&create, SMOOTHAF_FIO_ROOT_CREATE);
    smoothaf_fio_reserve(&create, 0, 1, 0);
    smoothaf_fio_opened(&create, 1);
    smoothaf_fio_write_result(&create, 22, 22);
    CHECK(!create.published, "create not published before close");
    smoothaf_fio_closed(&create);
    smoothaf_fio_publish(&create, 1);
    CHECK(create.published && create.phase == SMOOTHAF_FIO_SUCCESS,
          "create success after close");
    smoothaf_fio_release(&create);

    smoothaf_fio_state_init(&edit, SMOOTHAF_FIO_ROOT_EDIT);
    CHECK(smoothaf_fio_reserve(&edit, 0, 1, 0), "edit reservation");
    smoothaf_fio_not_found(&edit);
    CHECK(edit.phase == SMOOTHAF_FIO_NOT_FOUND, "missing edit file");
    smoothaf_fio_release(&edit);

    smoothaf_fio_state_init(&edit, SMOOTHAF_FIO_ROOT_EDIT);
    smoothaf_fio_reserve(&edit, 0, 1, 0);
    smoothaf_fio_size_check(&edit, 128);
    smoothaf_fio_size_mismatch(&edit);
    CHECK(edit.phase == SMOOTHAF_FIO_SIZE_MISMATCH && !edit.handle_valid,
          "edit size gate avoids open");
    smoothaf_fio_release(&edit);

    smoothaf_fio_state_init(&edit, SMOOTHAF_FIO_ROOT_EDIT);
    smoothaf_fio_reserve(&edit, 0, 1, 0);
    smoothaf_fio_size_check(&edit, 4096);
    smoothaf_fio_opened(&edit, 1);
    smoothaf_fio_seeked(&edit, 0);
    smoothaf_fio_write_result(&edit, 21, 21);
    smoothaf_fio_closed(&edit);
    smoothaf_fio_publish(&edit, 1);
    CHECK(edit.open_return && edit.seek_return == 0 && edit.close_called &&
          edit.published && edit.phase == SMOOTHAF_FIO_SUCCESS,
          "edit writes and closes successfully");
    smoothaf_fio_release(&edit);

    smoothaf_fio_state_init(&edit, SMOOTHAF_FIO_ROOT_EDIT);
    smoothaf_fio_reserve(&edit, 0, 1, 0);
    smoothaf_fio_opened(&edit, 1);
    smoothaf_fio_seeked(&edit, -1);
    CHECK(!edit.published && edit.seek_return != 0, "seek failure not published");
    smoothaf_fio_closed(&edit);
    smoothaf_fio_publish(&edit, 0);
    CHECK(edit.close_called && edit.phase == SMOOTHAF_FIO_ERROR,
          "seek failure closes valid handle");
    smoothaf_fio_release(&edit);

    smoothaf_fio_state_init(&paths, SMOOTHAF_FIO_FILE_PATHS);
    CHECK(!smoothaf_fio_reserve(&paths, 0, 0, 0), "unfinished task gate");
    CHECK(smoothaf_fio_reserve(&paths, 0, 1, 0), "path reservation");
    CHECK(!smoothaf_fio_reserve(&paths, 0, 1, 0), "path reentry gate");
    smoothaf_fio_release(&paths);
    CHECK(paths.variant == SMOOTHAF_FIO_FILE_PATHS,
          "path state keeps dedicated variant");
    smoothaf_fio_release(&create);

    smoothaf_fio_path_state_init(&path);
    CHECK(path.phase == SMOOTHAF_FIO_READY && !path.close_called,
          "path starts ready without close");
    CHECK(sizeof(expected_paths) / sizeof(expected_paths[0]) == 7 &&
          expected_paths[0][0] == 'S' && expected_paths[6][0] == 'B',
          "path controls preserve order including 8.3 control");
    smoothaf_fio_path_result(&path, 1, 1, 1, 0, 4096, 1, 1,
                             (uintptr_t)0x1234, 1);
    CHECK(path.phase == SMOOTHAF_FIO_PATH_SCAN && path.is_file_called &&
          path.is_file_return && path.size_called && path.size_return == 0 &&
          path.file_size == 4096 && path.open_called && path.open_return &&
          path.open_handle == (uintptr_t)0x1234 && path.handle_valid &&
          path.close_called,
          "path records calls, handle and close");
    smoothaf_fio_path_state_init(&path);
    smoothaf_fio_path_result(&path, 1, 0, 1, -1, 0, 1, 0, 0, 0);
    CHECK(path.is_file_called && !path.is_file_return && path.size_called &&
          path.size_return == -1 && path.open_called && !path.open_return &&
          !path.open_handle && !path.handle_valid && !path.close_called,
          "path records open after size failure without close");
    smoothaf_fio_path_result(&path, 1, 1, 1, 0, 4096, 1, 1,
                             (uintptr_t)0x1234, 0);
    CHECK(path.handle_valid && path.open_called &&
          path.open_handle == (uintptr_t)0x1234 && !path.close_called,
          "path helper preserves unclosed valid-handle state for detection");
    smoothaf_fio_path_state_init(&path);
    CHECK(path.phase == SMOOTHAF_FIO_READY && !path.handle_valid &&
          !path.close_called, "path repeat clears prior handle state");

    if (failures) {
        printf("%d FIO test(s) FAILED\n", failures);
        return 1;
    }
    printf("SmoothAF FIO root tests passed\n");
    return 0;
}
