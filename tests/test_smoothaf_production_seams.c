#include <stdio.h>
#include "../src/smoothaf_menu_toggle.h"
#include "../src/smoothaf_task_lifecycle.h"
#include "../src/smoothaf_runtime.h"

static int failures;
static int toggle_calls;
static int toggle_min;
static int toggle_max;

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("FAIL: %s\n", message); failures++; } } while (0)

static void mock_numeric_toggle(int *value, int delta, int min, int max)
{
    toggle_calls++;
    toggle_min = min;
    toggle_max = max;
    *value += delta;
    if (*value < min) *value = max;
    if (*value > max) *value = min;
}

static void test_logger_callback_contract(void)
{
    int logger_enabled = 0;
    int fio_enabled = 0;

    CHECK(smoothaf_logger_config_toggle(&logger_enabled, 1,
                                       mock_numeric_toggle),
          "logger selection accepts production priv/delta callback");
    CHECK(logger_enabled == 1 && toggle_calls == 1 &&
          toggle_min == 0 && toggle_max == 1,
          "Persistent Logger toggles OFF to ON through Legacy callback contract");
    CHECK(smoothaf_logger_config_toggle(&logger_enabled, -1,
                                       mock_numeric_toggle),
          "logger selection accepts reverse delta");
    CHECK(logger_enabled == 0,
          "Persistent Logger toggles ON to OFF through same callback contract");

    CHECK(smoothaf_logger_config_toggle(&fio_enabled, 1,
                                       mock_numeric_toggle),
          "FIO selection accepts production priv/delta callback");
    CHECK(fio_enabled == 1,
          "Logger FIO Enable toggles independently through same callback contract");
    CHECK(!smoothaf_logger_config_toggle(0, 1, mock_numeric_toggle) &&
          !smoothaf_logger_config_toggle(&fio_enabled, 1, 0),
          "invalid callback inputs do not authorize a logger change");
}

static void test_runtime_fio_lifecycle_state(void)
{
    struct smoothaf_runtime_state state;
    smoothaf_runtime_reset(&state);
    CHECK(state.fio_task_exited == 1,
          "reset state reports an exited consumer task");
}

static void test_task_failure_publication(void)
{
    int running = 0;
    int exited = 1;

    CHECK(smoothaf_task_reserve(&running, &exited),
          "task starter reserves lifecycle only after start checks");
    CHECK(running == 1 && exited == 0,
          "successful task reservation publishes running state");
    smoothaf_task_create_failed(&running, &exited);
    CHECK(running == 0 && exited == 1,
          "task-create failure publishes no-running and exited state");

    running = 0;
    exited = 1;
    CHECK(!smoothaf_task_reserve(0, &exited) && running == 0 && exited == 1,
          "semaphore failure before reservation leaves prior lifecycle state");
    CHECK(smoothaf_task_reserve(&running, &exited),
          "Monitoring retry can reserve after pre-task failure");
    smoothaf_task_create_failed(&running, &exited);
    CHECK(smoothaf_task_reserve(&running, &exited),
          "Movie Active Exp retry is not blocked by stale lifecycle state");
}

int main(void)
{
    test_logger_callback_contract();
    test_runtime_fio_lifecycle_state();
    test_task_failure_publication();
    if (failures) {
        printf("SmoothAF production seam tests failed: %d\n", failures);
        return 1;
    }
    printf("SmoothAF production seam tests passed\n");
    return 0;
}
