#include <stdio.h>
#include <string.h>
#include "../src/smoothaf_runtime.h"

static int failures;
static int create_calls;
static int take_calls;
static int give_calls;
static int task_calls;
static int stop_calls;
static int create_fails;
static int take_result;
static int give_result;
static int task_result;
static int invalid_handle;

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("FAIL: %s\n", message); failures++; } } while (0)

static void reset_mocks(void)
{
    create_calls = 0;
    take_calls = 0;
    give_calls = 0;
    task_calls = 0;
    stop_calls = 0;
    create_fails = 0;
    take_result = 0;
    give_result = 0;
    task_result = 1;
    invalid_handle = 0;
}

static void *mock_create_semaphore(const char *name, int initial_value)
{
    static int handle;
    CHECK(strcmp(name, "smoothaf_sem") == 0, "mandatory semaphore has stable name");
    CHECK(initial_value == 1, "mandatory semaphore starts available");
    create_calls++;
    if (create_fails) return 0;
    if (invalid_handle) return (void *)(uintptr_t)1;
    return &handle;
}

static int mock_take(void *semaphore, int timeout)
{
    CHECK(semaphore != 0, "acquisition never receives a null handle");
    CHECK(timeout == 0, "initialization validates without waiting");
    take_calls++;
    if ((uintptr_t)semaphore == 1u) return 1;
    return take_result;
}

static int mock_give(void *semaphore)
{
    CHECK(semaphore != 0, "release never receives a null handle");
    give_calls++;
    return give_result;
}

static void *mock_task(const char *name, uint32_t priority, uint32_t stack,
                       void *entry, void *arg)
{
    static int task;
    (void)entry;
    (void)arg;
    CHECK(strcmp(name, "smoothaf_fio") == 0, "optional task has stable name");
    CHECK(priority == 0x1d && stack == 0x1000, "optional task uses bounded resources");
    task_calls++;
    return task_result ? &task : 0;
}

static int mock_task_valid(void *task)
{
    return task != 0 && task_result;
}

static void mock_stop(void *task)
{
    CHECK(task != 0, "cleanup stops only a created task");
    stop_calls++;
}

static const struct smoothaf_runtime_ops OPS = {
    mock_create_semaphore, mock_take, mock_give,
    mock_task, mock_task_valid, mock_stop
};

static void test_creation_failure_is_safe(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    create_fails = 1;
    CHECK(!smoothaf_runtime_init(&state, &OPS, 0), "creation failure rejects runtime");
    CHECK(!state.ready && state.error == SMOOTHAF_RUNTIME_SEMAPHORE_CREATE,
          "creation failure leaves runtime unavailable");
    CHECK(state.first_error == SMOOTHAF_RUNTIME_SEMAPHORE_CREATE &&
          state.last_error == SMOOTHAF_RUNTIME_SEMAPHORE_CREATE,
          "creation failure is preserved as first and last error");
    CHECK(take_calls == 0 && give_calls == 0 && task_calls == 0,
          "creation failure cannot reach acquisition or tasks");
}

static void test_invalid_handle_is_safe(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    invalid_handle = 1;
    CHECK(!smoothaf_runtime_init(&state, &OPS, 0), "invalid handle rejects runtime");
    CHECK(!state.ready && state.error == SMOOTHAF_RUNTIME_SEMAPHORE_ACQUIRE,
          "invalid handle is caught before monitor startup");
    CHECK(!smoothaf_runtime_can_move(&state), "invalid handle cannot authorize movement");
}

static void test_acquisition_failure_is_safe(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    take_result = 1;
    CHECK(!smoothaf_runtime_init(&state, &OPS, 0), "acquisition failure rejects runtime");
    CHECK(!state.ready && state.error == SMOOTHAF_RUNTIME_SEMAPHORE_ACQUIRE,
          "acquisition failure leaves runtime unavailable");
    CHECK(task_calls == 0, "acquisition failure cannot create optional task");
    CHECK(!smoothaf_runtime_start_monitor(&state), "unready runtime cannot start monitor");
    smoothaf_runtime_stop(&state);
    smoothaf_runtime_panic(&state);
    CHECK(!smoothaf_runtime_can_move(&state),
          "STOP/PANIC after initialization failure cannot authorize movement");
}

static void test_release_failure_is_safe(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    give_result = 1;
    CHECK(!smoothaf_runtime_init(&state, &OPS, 0), "release failure rejects runtime");
    CHECK(!state.ready && state.error == SMOOTHAF_RUNTIME_SEMAPHORE_RELEASE,
          "release failure leaves runtime unavailable");
    CHECK(state.lock_held == 0 &&
          state.first_error == SMOOTHAF_RUNTIME_SEMAPHORE_RELEASE,
          "release failure clears held state and preserves first error");
}

static void test_fio_is_optional(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    task_result = 0;
    CHECK(smoothaf_runtime_init(&state, &OPS, 1),
          "FIO task failure does not reject monitor runtime");
    CHECK(state.ready && !state.fio_enabled &&
          state.error == SMOOTHAF_RUNTIME_FIO_TASK &&
          state.first_error == SMOOTHAF_RUNTIME_FIO_TASK &&
          state.last_error == SMOOTHAF_RUNTIME_FIO_TASK,
          "FIO failure degrades to disabled sink and preserves error");
    CHECK(smoothaf_runtime_start_monitor(&state), "monitor starts with FIO unavailable");
    CHECK(smoothaf_runtime_can_move(&state), "movement gate is open only after monitor start");
    smoothaf_runtime_shutdown(&state, &OPS);
    CHECK(!state.ready && stop_calls == 0 && !smoothaf_runtime_can_move(&state),
          "partial FIO failure cleanup leaves no task or movement authorization");
}

static void test_fio_off_never_creates_task(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    CHECK(smoothaf_runtime_init(&state, &OPS, 0), "FIO-off initialization succeeds");
    CHECK(task_calls == 0 && state.ready && !state.fio_enabled,
          "FIO-off initialization does not create optional task");
    CHECK(smoothaf_runtime_start_monitor(&state), "FIO-off monitor starts");
}

static void test_partial_cleanup_stop_panic_and_no_move(void)
{
    struct smoothaf_runtime_state state;
    reset_mocks();
    CHECK(smoothaf_runtime_init(&state, &OPS, 1), "FIO-on initialization succeeds");
    CHECK(state.fio_task_created, "successful FIO task is tracked");
    smoothaf_runtime_shutdown(&state, &OPS);
    CHECK(stop_calls == 1 && !state.ready && !smoothaf_runtime_can_move(&state),
          "partial cleanup disables movement and stops optional task");

    reset_mocks();
    CHECK(smoothaf_runtime_init(&state, &OPS, 0), "runtime can be reinitialized");
    CHECK(smoothaf_runtime_start_monitor(&state), "monitor starts before panic");
    smoothaf_runtime_panic(&state);
    CHECK(!smoothaf_runtime_can_move(&state), "PANIC closes movement gate");
    smoothaf_runtime_stop(&state);
    CHECK(!smoothaf_runtime_can_move(&state), "STOP keeps movement gate closed");
}

int main(void)
{
    test_creation_failure_is_safe();
    test_invalid_handle_is_safe();
    test_acquisition_failure_is_safe();
    test_release_failure_is_safe();
    test_fio_is_optional();
    test_fio_off_never_creates_task();
    test_partial_cleanup_stop_panic_and_no_move();
    if (failures) {
        printf("SmoothAF runtime tests failed: %d\n", failures);
        return 1;
    }
    printf("SmoothAF runtime tests passed\n");
    return 0;
}
