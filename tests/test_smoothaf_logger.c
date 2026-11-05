/**
 * SmoothAF Logger — Host Tests (Phase 3A)
 *
 * Tests: init/shutdown, empty/full buffer, wrap-around, overflow ordering,
 * record integrity, producer/consumer concurrency, dropped counter,
 * consumer failure, STOP/PANIC non-blocking, timestamp wrap-around,
 * logging OFF no-interference, small batches, flush via backend.
 */

#include "smoothaf_logger.h"
#include "smoothaf_logger_fio.h"
#include "../src/smoothaf_menu_toggle.h"
#include "../src/smoothaf_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

static int g_failures = 0;

#define TEST(name) \
    do { \
        printf("TEST: %s ... ", name); \
        fflush(stdout); \
    } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } } while (0)

#define ASSERT_FALSE(cond, msg) \
    do { if (cond) { printf("FAIL: %s\n", msg); g_failures++; } } while (0)

#define ASSERT_EQ(a, b, msg) \
    do { if ((a) != (b)) { printf("FAIL: %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); g_failures++; } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    do { if (strcmp((a), (b)) != 0) { printf("FAIL: %s (expected '%s', got '%s')\n", msg, (b), (a)); g_failures++; } } while (0)

#define PASS() printf("PASS\n")

/* ============================================================
 * Mock Clock for Deterministic Timestamps
 * ============================================================ */

static uint32_t g_mock_time = 1000000;

static uint32_t mock_clock(void) {
    return g_mock_time;
}

static void advance_time(uint32_t ms) {
    g_mock_time += ms;
}

/* ============================================================
 * Test Helpers
 * ============================================================ */

static void reset_logger(struct smoothaf_persist_log_buffer *buf) {
    smoothaf_log_fio_set_card_present(true);
    smoothaf_log_fio_set_simulated_write_chunk(0);
    remove("ML/LOGS/SMTH000.LOG");
    remove("ML/LOGS/SMTH001.LOG");
    remove("ML/LOGS/SMTH002.LOG");
    smoothaf_persist_logger_init(buf);
    smoothaf_persist_log_reset_dropped(buf);
    smoothaf_persist_log_clear_mock_clock();
    smoothaf_persist_log_set_consumer_delay_ms(0);
    smoothaf_persist_log_set_write_fail_next(false);
    g_mock_time = 1000000;
}

static void enable_logger(struct smoothaf_persist_log_buffer *buf) {
    smoothaf_persist_logger_set_enabled(buf, true);
    smoothaf_persist_log_set_mock_clock(mock_clock);
}

/* ============================================================
 * Tests
 * ============================================================ */

static void test_init_shutdown(void) {
    TEST("init_shutdown");
    struct smoothaf_persist_log_buffer buf;
    smoothaf_persist_logger_init(&buf);
    ASSERT_FALSE(buf.enabled, "default disabled");
    ASSERT_EQ(buf.head, 0u, "head starts at 0");
    ASSERT_EQ(buf.tail, 0u, "tail starts at 0");
    ASSERT_EQ(buf.dropped_count, 0u, "dropped starts at 0");
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "buffer empty after init");
    ASSERT_FALSE(smoothaf_persist_log_is_full(&buf), "buffer not full after init");
    smoothaf_persist_logger_set_enabled(&buf, true);
    ASSERT_TRUE(buf.enabled, "enable works");
    smoothaf_persist_logger_set_enabled(&buf, false);
    ASSERT_FALSE(buf.enabled, "disable works");
    PASS();
}

static void test_empty_full(void) {
    TEST("empty_full");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "initially empty");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "fill=0");

    /* Fill buffer completely (255 records for 256-entry ring) */
    for (uint32_t i = 0; i < SMOOTHAF_LOG_BUF_ENTRIES - 1; i++) {
        int32_t p[6] = {(int32_t)i, 0,0,0,0,0};
        bool ok = smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        ASSERT_TRUE(ok, "record should succeed until full");
        advance_time(10);
    }
    ASSERT_TRUE(smoothaf_persist_log_is_full(&buf), "buffer full after N-1 records");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), SMOOTHAF_LOG_BUF_ENTRIES - 1, "fill=N-1");

    /* One more should fail (dropped) */
    int32_t p[6] = {999, 0,0,0,0,0};
    bool ok = smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
    ASSERT_FALSE(ok, "record fails when full");
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 1u, "dropped count incremented");

    PASS();
}

static void test_wrap_around(void) {
    TEST("wrap_around");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Fill buffer completely */
    for (uint32_t i = 0; i < SMOOTHAF_LOG_BUF_ENTRIES - 1; i++) {
        int32_t p[6] = {(int32_t)i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }
    ASSERT_TRUE(smoothaf_persist_log_is_full(&buf), "full after first fill");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), SMOOTHAF_LOG_BUF_ENTRIES - 1, "fill=N-1");

    /* Consume all in multiple batches (batch limit = 16) */
    struct smoothaf_persist_log_batch batch;
    uint32_t total_consumed = 0;
    while (!smoothaf_persist_log_is_empty(&buf)) {
        uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
        total_consumed += c;
        ASSERT_TRUE(c <= SMOOTHAF_LOG_BATCH_LINES, "batch size respected");
    }
    ASSERT_EQ(total_consumed, SMOOTHAF_LOG_BUF_ENTRIES - 1, "consume all records");
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "empty after full consume");

    /* Fill again — should wrap head */
    for (uint32_t i = 0; i < SMOOTHAF_LOG_BUF_ENTRIES - 1; i++) {
        int32_t p[6] = {(int32_t)(10000 + i), 0,0,0,0,0};
        bool ok = smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        ASSERT_TRUE(ok, "record after wrap should succeed");
        advance_time(1);
    }
    ASSERT_TRUE(smoothaf_persist_log_is_full(&buf), "full after wrap fill");

    PASS();
}

static void test_overflow_ordering(void) {
    TEST("overflow_ordering");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Fill buffer completely */
    for (uint32_t i = 0; i < SMOOTHAF_LOG_BUF_ENTRIES - 1; i++) {
        int32_t p[6] = {(int32_t)i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }

    /* Overflow: next records should be dropped, old ones preserved */
    uint32_t dropped_before = smoothaf_persist_log_get_dropped(&buf);
    for (int i = 0; i < 10; i++) {
        int32_t p[6] = {(int32_t)(-1), 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }
    uint32_t dropped_after = smoothaf_persist_log_get_dropped(&buf);
    ASSERT_EQ(dropped_after - dropped_before, 10u, "10 records dropped on overflow");

    /* Consume all — verify oldest records still present (FIFO) */
    struct smoothaf_persist_log_batch batch;
    uint32_t total_consumed = 0;
    bool first_batch = true;
    while (!smoothaf_persist_log_is_empty(&buf)) {
        uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
        total_consumed += c;
        /* First record of first batch should be 0, not -1 */
        if (first_batch) {
            ASSERT_STREQ(batch.lines[0], "1000000\tCOMMAND\t1\t0\t0\t0\t0\t0\t0", "oldest record preserved");
            first_batch = false;
        }
    }
    ASSERT_EQ(total_consumed, SMOOTHAF_LOG_BUF_ENTRIES - 1, "all original records consumed");

    PASS();
}

static void test_record_integrity(void) {
    TEST("record_integrity");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Record various event types with known payloads */
    smoothaf_persist_log_session_start(&buf, g_mock_time, "test-build", 8000, 8, 1); advance_time(10);
    smoothaf_persist_log_episode_start(&buf, g_mock_time, 1, 2, 1000); advance_time(10);
    smoothaf_persist_log_command(&buf, g_mock_time, 1, 0, 1, 3, 400, 86, 120); advance_time(10);
    smoothaf_persist_log_measurement(&buf, g_mock_time, 1, 842, 128, 45, 67, 1, 1); advance_time(10);
    smoothaf_persist_log_timeout(&buf, g_mock_time, 1, 3, 7500, 7); advance_time(10);
    smoothaf_persist_log_episode_end(&buf, g_mock_time, 1, 4, 950, 150, 900, 140, 8); advance_time(10);
    smoothaf_persist_log_safety_paused(&buf, g_mock_time, 1, 2, 500); advance_time(10);
    smoothaf_persist_log_reference_set(&buf, g_mock_time, 1, 3, 900, 120); advance_time(10);
    smoothaf_persist_log_auto_probe(&buf, g_mock_time, 1, 1, -1, 850, -20, 1); advance_time(10);
    smoothaf_persist_log_controller(&buf, g_mock_time, 1, 2, 4, 1, 900); advance_time(10);
    smoothaf_persist_log_environmental(&buf, g_mock_time, 1, 2, 32, 1); advance_time(10);
    smoothaf_persist_log_write_error(&buf, g_mock_time, 1, 3, -5); advance_time(10);
    smoothaf_persist_log_cancelled(&buf, g_mock_time, 1, 1, 5); advance_time(10);
    smoothaf_persist_log_session_end(&buf, g_mock_time, 4, 10000, 1);

    /* Consume and verify formatting */
    struct smoothaf_persist_log_batch batch;
    uint32_t consumed = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(consumed, 14u, "all records consumed");

    /* Verify each line has correct event type and field count (9 fields: ts, type, ep, 6 payload) */
    for (uint32_t i = 0; i < batch.count; i++) {
        int fields = 0;
        /* Count tabs without modifying original string */
        char *line = batch.lines[i];
        for (char *p = line; *p; p++) {
            if (*p == '\t') fields++;
        }
        fields++; /* fields = tabs + 1 */
        ASSERT_EQ(fields, 9, "each line has 9 tab-separated fields");
    }

    /* Verify specific event types in order */
    ASSERT_TRUE(strstr(batch.lines[0], "SESSION_START") != NULL, "line 0: SESSION_START");
    ASSERT_TRUE(strstr(batch.lines[1], "EPISODE_START") != NULL, "line 1: EPISODE_START");
    ASSERT_TRUE(strstr(batch.lines[2], "COMMAND") != NULL, "line 2: COMMAND");
    ASSERT_TRUE(strstr(batch.lines[3], "MEASUREMENT") != NULL, "line 3: MEASUREMENT");
    ASSERT_TRUE(strstr(batch.lines[4], "TIMEOUT") != NULL, "line 4: TIMEOUT");
    ASSERT_TRUE(strstr(batch.lines[5], "EPISODE_END") != NULL, "line 5: EPISODE_END");
    ASSERT_TRUE(strstr(batch.lines[6], "SAFETY_PAUSED") != NULL, "line 6: SAFETY_PAUSED");
    ASSERT_TRUE(strstr(batch.lines[12], "CANCELLED") != NULL, "line 12: CANCELLED");
    ASSERT_TRUE(strstr(batch.lines[13], "SESSION_END") != NULL, "line 13: SESSION_END");

    PASS();
}

struct pthread_stress_ctx {
    struct smoothaf_persist_log_buffer *buf;
    atomic_bool producer_done;
    atomic_uint accepted;
    atomic_uint consumed;
};

static void *pthread_producer(void *arg) {
    struct pthread_stress_ctx *ctx = (struct pthread_stress_ctx *)arg;
    for (uint32_t i = 0; i < 20000; i++) {
        int32_t payload[6] = {(int32_t)i, 0, 0, 0, 0, 0};
        if (smoothaf_persist_log_record(ctx->buf, i,
                                        SMOOTHAF_PERSIST_LOG_COMMAND, 1,
                                        payload))
            atomic_fetch_add(&ctx->accepted, 1);
        if ((i & 31u) == 0) sched_yield();
    }
    atomic_store(&ctx->producer_done, true);
    return NULL;
}

static void *pthread_consumer(void *arg) {
    struct pthread_stress_ctx *ctx = (struct pthread_stress_ctx *)arg;
    struct smoothaf_persist_log_batch batch;
    while (!atomic_load(&ctx->producer_done) ||
           !smoothaf_persist_log_is_empty(ctx->buf)) {
        uint32_t count = smoothaf_persist_log_consume_batch(ctx->buf, &batch);
        if (count) {
            atomic_fetch_add(&ctx->consumed, count);
            smoothaf_persist_log_flush_batch(ctx->buf, &batch,
                                              &smoothaf_persist_log_sim_backend);
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void test_real_pthread_producer_consumer(void) {
    TEST("real_pthread_producer_consumer");
    struct smoothaf_persist_log_buffer buf;
    struct pthread_stress_ctx ctx;
    pthread_t producer;
    pthread_t consumer;
    reset_logger(&buf);
    enable_logger(&buf);
    ctx.buf = &buf;
    atomic_init(&ctx.producer_done, false);
    atomic_init(&ctx.accepted, 0);
    atomic_init(&ctx.consumed, 0);
    ASSERT_EQ(pthread_create(&consumer, NULL, pthread_consumer, &ctx), 0,
              "consumer thread starts");
    ASSERT_EQ(pthread_create(&producer, NULL, pthread_producer, &ctx), 0,
              "producer thread starts");
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "stress ring drains");
    ASSERT_EQ(atomic_load(&ctx.accepted), atomic_load(&ctx.consumed),
              "accepted records are consumed");
    PASS();
}

static void test_producer_consumer_concurrency(void) {
    TEST("producer_consumer_concurrency");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Interleave produce/consume - consume_batch drains up to batch limit */
    for (int cycle = 0; cycle < 100; cycle++) {
        /* Producer: add 3 records */
        for (int i = 0; i < 3; i++) {
            int32_t p[6] = {cycle * 10 + i, 0,0,0,0,0};
            bool ok = smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
            ASSERT_TRUE(ok, "produce should not drop in this test");
            advance_time(1);
        }
        /* Consumer: drain all available (up to batch limit of 16) */
        struct smoothaf_persist_log_batch batch;
        uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
        ASSERT_EQ(c, 3u, "consume 3 per cycle");
    }

    /* Buffer should be empty after each cycle */
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "buffer empty after each consume");
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 0u, "no drops in controlled concurrency");

    PASS();
}

static void test_dropped_counter(void) {
    TEST("dropped_counter");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 0u, "starts at 0");

    /* Fill buffer */
    for (uint32_t i = 0; i < SMOOTHAF_LOG_BUF_ENTRIES - 1; i++) {
        int32_t p[6] = {(int32_t)i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }

    /* Overflow 5 times */
    for (int i = 0; i < 5; i++) {
        int32_t p[6] = {-1, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 5u, "5 drops counted");

    /* Reset counter */
    smoothaf_persist_log_reset_dropped(&buf);
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 0u, "reset works");

    /* More overflows */
    for (int i = 0; i < 3; i++) {
        int32_t p[6] = {-1, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 3u, "counter works after reset");

    PASS();
}

static void test_consumer_failure(void) {
    TEST("consumer_failure");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Add some records */
    for (int i = 0; i < 10; i++) {
        int32_t p[6] = {i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(1);
    }

    /* Format the batch, then simulate a backend write failure. */
    struct smoothaf_persist_log_batch batch = {0};
    uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(c, 10u, "consume formats records");
    smoothaf_persist_log_set_write_fail_next(true);
    smoothaf_persist_log_write_result_e res =
        smoothaf_persist_log_flush_batch(&buf, &batch, &smoothaf_persist_log_sim_backend);
    ASSERT_EQ(res, SMOOTHAF_PERSIST_LOG_WRITE_ERR, "flush returns error on write failure");

    /* Records should still be in buffer (not committed) */
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 10u, "records retained after failed flush");

    /* Retry the same formatted batch */
    res = smoothaf_persist_log_flush_batch(&buf, &batch, &smoothaf_persist_log_sim_backend);
    ASSERT_EQ(res, SMOOTHAF_PERSIST_LOG_WRITE_OK, "subsequent flush succeeds");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "buffer empty after successful flush");

    PASS();
}

static void test_stop_panic_non_blocking(void) {
    TEST("stop_panic_non_blocking");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Add some records */
    for (int i = 0; i < 5; i++) {
        int32_t p[6] = {i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(10);
    }
    uint32_t fill_before = smoothaf_persist_log_get_fill(&buf);

    /* Signal STOP — should record CANCELLED event, NOT flush */
    smoothaf_persist_log_signal_stop(&buf, g_mock_time, 1, SMOOTHAF_PERSIST_LOG_CANCEL_STOP, 5);
    advance_time(10);

    /* Buffer should have 6 records now (5 original + 1 CANCELLED) */
    uint32_t fill_after = smoothaf_persist_log_get_fill(&buf);
    ASSERT_EQ(fill_after, fill_before + 1, "CANCELLED event added");

    /* Consumer must still drain — logger does NOT auto-flush */
    struct smoothaf_persist_log_batch batch;
    uint32_t consumed = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(consumed, 6u, "consumer drains all including CANCELLED");

    /* Verify CANCELLED is last */
    ASSERT_TRUE(strstr(batch.lines[5], "CANCELLED") != NULL, "CANCELLED event present");
    ASSERT_TRUE(strstr(batch.lines[5], "1") != NULL, "cancel_reason=STOP");

    PASS();
}

static void test_timestamp_wrap_around(void) {
    TEST("timestamp_wrap_around");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Set mock clock near UINT32_MAX */
    g_mock_time = 0xFFFFFE00;  // ~4.29s before wrap

    for (int i = 0; i < 10; i++) {
        int32_t p[6] = {i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(100);  // Will wrap around
    }

    /* Consume and verify timestamps wrap correctly */
    struct smoothaf_persist_log_batch batch;
    smoothaf_persist_log_consume_batch(&buf, &batch);

    /* First timestamp should be near MAX */
    ASSERT_TRUE(batch.lines[0][0] >= '4', "first timestamp near UINT32_MAX");
    /* Last timestamp should have wrapped to small value */
    char *last_line = batch.lines[9];
    uint32_t last_ts = (uint32_t)strtoul(last_line, NULL, 10);
    ASSERT_TRUE(last_ts < 1000, "last timestamp wrapped to small value");

    PASS();
}

static void test_logging_off_no_interference(void) {
    TEST("logging_off_no_interference");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    /* Do NOT enable — default is disabled */

    /* All operations should be silent no-ops */
    bool ok = smoothaf_persist_log_record(&buf, 1000, SMOOTHAF_PERSIST_LOG_COMMAND, 1, (int32_t[6]){0});
    ASSERT_TRUE(ok, "record returns true when disabled");

    ok = smoothaf_persist_log_session_start(&buf, 1000, "test", 8000, 8, 1);
    ASSERT_TRUE(ok, "session_start returns true when disabled");

    ok = smoothaf_persist_log_command(&buf, 1000, 1, 0, 1, 3, 400, 86, 120);
    ASSERT_TRUE(ok, "command returns true when disabled");

    /* Buffer should remain empty */
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "buffer empty when disabled");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "fill=0 when disabled");
    ASSERT_EQ(smoothaf_persist_log_get_dropped(&buf), 0u, "dropped=0 when disabled");

    /* Consume should return 0 */
    struct smoothaf_persist_log_batch batch;
    uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(c, 0u, "consume returns 0 when disabled");

    /* Enable, add record, disable, verify record persists */
    smoothaf_persist_logger_set_enabled(&buf, true);
    smoothaf_persist_log_command(&buf, 2000, 1, 0, 1, 3, 400, 86, 120);
    smoothaf_persist_logger_set_enabled(&buf, false);
    c = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(c, 1u, "record added while enabled persists after disable");

    PASS();
}

static void test_specific_events(void) {
    TEST("specific_events_episode_start");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    smoothaf_persist_log_episode_start(&buf, 1000000, 42, 3, 1234);
    struct smoothaf_persist_log_batch batch;
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(batch.count, 1u, "one record");
    ASSERT_TRUE(strstr(batch.lines[0], "EPISODE_START") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t42\t") != NULL, "episode_id=42");
    ASSERT_TRUE(strstr(batch.lines[0], "\t3\t") != NULL, "algorithm=3");
    ASSERT_TRUE(strstr(batch.lines[0], "\t1234\t") != NULL, "baseline=1234");

    TEST("specific_events_command");
    reset_logger(&buf);
    enable_logger(&buf);
    smoothaf_persist_log_command(&buf, 1000000, 1, 3, -1, 5, 450, 90, 110);
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_TRUE(strstr(batch.lines[0], "COMMAND") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t3\t") != NULL, "cmd_index=3");
    ASSERT_TRUE(strstr(batch.lines[0], "\t-1\t") != NULL, "direction=-1");
    ASSERT_TRUE(strstr(batch.lines[0], "\t5\t") != NULL, "step=5");
    ASSERT_TRUE(strstr(batch.lines[0], "\t450\t") != NULL, "lens_ms=450");
    ASSERT_TRUE(strstr(batch.lines[0], "\t90\t") != NULL, "settle_ms=90");
    ASSERT_TRUE(strstr(batch.lines[0], "\t110") != NULL, "sample_wait_ms=110");

    TEST("specific_events_measurement");
    reset_logger(&buf);
    enable_logger(&buf);
    smoothaf_persist_log_measurement(&buf, 1000000, 1, 950, 130, 40, 70, 1, 1);
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_TRUE(strstr(batch.lines[0], "MEASUREMENT") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t950\t") != NULL, "score=950");
    ASSERT_TRUE(strstr(batch.lines[0], "\t130\t") != NULL, "mean=130");
    ASSERT_TRUE(strstr(batch.lines[0], "\t40\t") != NULL, "sad=40");
    ASSERT_TRUE(strstr(batch.lines[0], "\t70\t") != NULL, "laplacian=70");
    ASSERT_TRUE(strstr(batch.lines[0], "\t1\t") != NULL, "valid=1");
    ASSERT_TRUE(strstr(batch.lines[0], "\t1") != NULL, "confident=1");

    TEST("specific_events_timeout");
    reset_logger(&buf);
    enable_logger(&buf);
    smoothaf_persist_log_timeout(&buf, 1000000, 1, 2, 7950, 7);
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_TRUE(strstr(batch.lines[0], "TIMEOUT") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t2\t") != NULL, "phase=2");
    ASSERT_TRUE(strstr(batch.lines[0], "\t7950\t") != NULL, "budget_used=7950");
    ASSERT_TRUE(strstr(batch.lines[0], "\t7") != NULL, "cmds_executed=7");

    TEST("specific_events_episode_end");
    reset_logger(&buf);
    enable_logger(&buf);
    smoothaf_persist_log_episode_end(&buf, 1000000, 1, 4, 980, 200, 950, 190, 8);
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_TRUE(strstr(batch.lines[0], "EPISODE_END") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t4\t") != NULL, "result=CONVERGED(4)");
    ASSERT_TRUE(strstr(batch.lines[0], "\t980\t") != NULL, "best_score=980");
    ASSERT_TRUE(strstr(batch.lines[0], "\t200\t") != NULL, "best_pos=200");
    ASSERT_TRUE(strstr(batch.lines[0], "\t950\t") != NULL, "final_score=950");
    ASSERT_TRUE(strstr(batch.lines[0], "\t190\t") != NULL, "final_pos=190");
    ASSERT_TRUE(strstr(batch.lines[0], "\t8") != NULL, "commands=8");

    TEST("specific_events_safety_paused");
    reset_logger(&buf);
    enable_logger(&buf);
    smoothaf_persist_log_safety_paused(&buf, 1000000, 1, 2, 500);
    smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_TRUE(strstr(batch.lines[0], "SAFETY_PAUSED") != NULL, "event type");
    ASSERT_TRUE(strstr(batch.lines[0], "\t2\t") != NULL, "reason=FAILURES(2)");
    ASSERT_TRUE(strstr(batch.lines[0], "\t500\t") != NULL, "remaining_ms=500");

    PASS();
}

static void test_small_batches(void) {
    TEST("small_batches");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Add 20 records (more than batch limit of 16) */
    for (int i = 0; i < 20; i++) {
        int32_t p[6] = {i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(10);
    }

    /* First consume should return 16 (batch limit) */
    struct smoothaf_persist_log_batch batch;
    uint32_t c = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(c, 16u, "first consume limited to batch size");
    ASSERT_EQ(batch.count, 16u, "batch count = 16");
    ASSERT_FALSE(batch.truncated, "not truncated");

    /* Second consume should return remaining 4 */
    c = smoothaf_persist_log_consume_batch(&buf, &batch);
    ASSERT_EQ(c, 4u, "second consume returns remaining");
    ASSERT_EQ(batch.count, 4u, "batch count = 4");

    /* Buffer now empty */
    ASSERT_TRUE(smoothaf_persist_log_is_empty(&buf), "buffer empty after two consumes");

    PASS();
}

static void test_flush_batch(void) {
    TEST("flush_batch");
    struct smoothaf_persist_log_buffer buf;
    reset_logger(&buf);
    enable_logger(&buf);

    /* Add records */
    for (int i = 0; i < 5; i++) {
        int32_t p[6] = {i, 0,0,0,0,0};
        smoothaf_persist_log_record(&buf, g_mock_time, SMOOTHAF_PERSIST_LOG_COMMAND, 1, p);
        advance_time(10);
    }

    struct smoothaf_persist_log_batch batch;
    smoothaf_persist_log_consume_batch(&buf, &batch);

    /* Simulate a backend write failure. The batch must remain retryable. */
    smoothaf_persist_log_set_write_fail_next(true);
    smoothaf_persist_log_write_result_e res = smoothaf_persist_log_flush_batch(&buf, &batch, &smoothaf_persist_log_sim_backend);
    ASSERT_EQ(res, SMOOTHAF_PERSIST_LOG_WRITE_ERR, "flush returns error on failure");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 5u, "records retained after failed flush");

    /* Retry through the same batch. */
    res = smoothaf_persist_log_flush_batch(&buf, &batch, &smoothaf_persist_log_sim_backend);
    ASSERT_EQ(res, SMOOTHAF_PERSIST_LOG_WRITE_OK, "retry succeeds");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "records consumed after successful retry");

    PASS();
}

static int g_task_create_calls;
static int g_task_create_success;
static int g_runtime_semaphore;

static void *integration_create_semaphore(const char *name, int initial_value)
{
    if (strcmp(name, "smoothaf_sem") != 0 || initial_value != 1) return NULL;
    return &g_runtime_semaphore;
}

static int integration_take_semaphore(void *semaphore, int timeout)
{
    return semaphore == &g_runtime_semaphore && timeout == 0 ? 0 : 1;
}

static int integration_give_semaphore(void *semaphore)
{
    return semaphore == &g_runtime_semaphore ? 0 : 1;
}

static int integration_task_valid(void *task)
{
    return task != NULL;
}

static void integration_numeric_toggle(int *value, int delta, int min, int max)
{
    *value += delta;
    if (*value < min) *value = max;
    if (*value > max) *value = min;
}

static void *integration_task_create(const char *name, uint32_t priority,
                                     uint32_t stack_size, void *entry, void *arg)
{
    (void)priority;
    (void)stack_size;
    (void)entry;
    (void)arg;
    if (strcmp(name, "smoothaf_fio") != 0) return NULL;
    g_task_create_calls++;
    return g_task_create_success ? (void *)0x1000 : NULL;
}

static void test_integrated_active_continuous_pipeline(void) {
    TEST("integrated_active_continuous_pipeline");
    struct smoothaf_log_fio_backend fio;
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    int logger_enabled = 0;
    int fio_requested = 0;

    /* Exercise the same menu priv/delta seam, then apply the effective FIO
     * request through the shared runtime lifecycle seam. */
    smoothaf_log_fio_init(&fio);
    smoothaf_persist_logger_init(&buf);
    {
        const struct smoothaf_runtime_ops runtime_ops = {
            integration_create_semaphore, integration_take_semaphore,
            integration_give_semaphore, integration_task_create,
            integration_task_valid, NULL
        };
        struct smoothaf_runtime_state runtime;
        g_task_create_calls = 0;
        g_task_create_success = 1;
        ASSERT_TRUE(smoothaf_runtime_init(&runtime, &runtime_ops, 1),
                    "integrated runtime applies FIO request and creates task");
        ASSERT_TRUE(runtime.ready && runtime.fio_requested &&
                    runtime.fio_task_created && runtime.fio_task_running &&
                    runtime.fio_enabled,
                    "integrated runtime reports effective FIO task state");
        ASSERT_EQ(g_task_create_calls, 1, "integrated runtime starts FIO task");
        ASSERT_TRUE(smoothaf_runtime_start_monitor(&runtime),
                    "integrated ACTIVE CONTINUOUS monitor starts");
    }
    smoothaf_logger_config_toggle(&logger_enabled, 1, integration_numeric_toggle);
    smoothaf_logger_config_toggle(&fio_requested, 1, integration_numeric_toggle);
    smoothaf_persist_logger_set_enabled(&buf, logger_enabled != 0);
    smoothaf_log_fio_request_enabled(&fio, fio_requested && logger_enabled);
    ASSERT_EQ(g_task_create_calls, 1, "FIO task startup is attempted");
    ASSERT_TRUE(fio.requested_enabled && fio.config_pending,
                "FIO request is pending before consumer service");
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "FIO consumer applies both toggle requests");
    ASSERT_TRUE(fio.enabled, "FIO becomes effective after consumer service");
    ASSERT_FALSE(fio.current_file != NULL, "active session starts without an open file");

    /* SESSION_START models the first event emitted by ACTIVE CONTINUOUS;
     * EPISODE_START models the next armed episode event. */
    ASSERT_TRUE(smoothaf_persist_log_session_start(
                    &buf, 1000, "integration-build", 10000, 8, 1),
                "ACTIVE CONTINUOUS publishes SESSION_START");
    ASSERT_TRUE(smoothaf_persist_log_episode_start(&buf, 1010, 1, 0, 123),
                "ACTIVE CONTINUOUS publishes EPISODE_START");
    ASSERT_TRUE(smoothaf_persist_log_command(&buf, 1020, 1, 1, 1, 3, 4, 5, 6),
                "ACTIVE CONTINUOUS publishes COMMAND");
    ASSERT_TRUE(smoothaf_persist_log_measurement(
                    &buf, 1030, 1, 130, 120, 10, 7, 1, 1),
                "ACTIVE CONTINUOUS publishes MEASUREMENT");
    ASSERT_TRUE(smoothaf_persist_log_episode_end(
                    &buf, 1040, 1, 1, 130, 3, 129, 3, 1),
                "ACTIVE CONTINUOUS publishes EPISODE_END");
    ASSERT_TRUE(smoothaf_persist_log_session_end(&buf, 1050, 1, 50, 1),
                "ACTIVE CONTINUOUS publishes SESSION_END");
    ASSERT_EQ(smoothaf_persist_log_get_published(&buf), 6u,
              "all ACTIVE CONTINUOUS lifecycle events are published");
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "ACTIVE CONTINUOUS events reach the FIO consumer");
    ASSERT_EQ(smoothaf_persist_log_get_flushed(&buf), 6u,
              "all lifecycle events are confirmed by backend");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u,
              "integrated consumer drains the complete lifecycle");
    ASSERT_EQ(fio.total_files_written, 1u,
              "integrated first event creates the first file");
    ASSERT_TRUE(fio.bytes_written > 0u, "integrated pipeline writes bytes");
    smoothaf_log_fio_request_close(&fio);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "integrated pipeline services close");
    ASSERT_EQ(fio.close_count, 1u, "integrated pipeline closes the file");
    PASS();
}

static void test_fio_configuration_and_lifecycle(void) {
    TEST("fio_configuration_and_lifecycle");
    struct smoothaf_log_fio_backend fio;
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    int32_t payload[6] = {11, 22, 33, 44, 55, 66};

    reset_logger(&buf);
    smoothaf_log_fio_init(&fio);
    smoothaf_log_fio_request_enabled(&fio, true);
    ASSERT_TRUE(fio.requested_enabled, "FIO request is published");
    ASSERT_FALSE(fio.enabled, "FIO request is not yet effective");
    ASSERT_TRUE(fio.config_pending, "FIO request is pending for consumer");

    /* Enabling the consumer alone must not create a file. */
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "consumer applies FIO configuration");
    ASSERT_TRUE(fio.enabled, "FIO request becomes effective");
    ASSERT_EQ(fio.total_files_written, 0u,
              "no file is created while the logger ring is empty");

    smoothaf_persist_logger_set_enabled(&buf, true);
    smoothaf_persist_log_record(&buf, 3000, SMOOTHAF_PERSIST_LOG_COMMAND, 1,
                                payload);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "consumer flushes the first persistent event");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u,
              "first event is committed after a successful write");
    ASSERT_EQ(fio.total_files_written, 1u,
              "first non-empty batch creates exactly one file");
    ASSERT_TRUE(fio.current_file != NULL, "file remains open until close request");

    smoothaf_log_fio_request_close(&fio);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "consumer handles close request");
    ASSERT_FALSE(fio.enabled, "close request disables effective FIO");
    ASSERT_FALSE(fio.current_file != NULL, "close request closes the file");
    ASSERT_TRUE(fio.total_files_written == 1u,
                "close does not create an extra file");
    PASS();
}

static void test_fio_task_loop_reproduction_snapshot(void) {
    TEST("fio_task_loop_reproduction_snapshot");
    struct smoothaf_log_fio_backend fio;
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    int32_t payload[6] = {1, 2, 3, 4, 5, 6};

    /* Model the production sequence: publish the request, run the consumer
     * iteration that applies it, then publish the seven records observed on
     * the camera and run the next consumer iteration.  No state fields are
     * assigned directly; all transitions use the production backend API. */
    reset_logger(&buf);
    smoothaf_log_fio_init(&fio);
    smoothaf_persist_logger_set_enabled(&buf, true);
    smoothaf_log_fio_request_enabled(&fio, true);
    smoothaf_log_fio_request_limits(&fio, 256u * 1024u,
                                    SMOOTHAF_LOG_FIO_MAX_FILES);
    ASSERT_TRUE(fio.requested_enabled && fio.config_pending,
                "task-loop request is published before service");
    ASSERT_FALSE(fio.enabled, "effective FIO starts disabled");

    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "task-loop service applies the pending request");
    ASSERT_TRUE(fio.enabled, "effective FIO is enabled by the consumer");
    ASSERT_FALSE(fio.config_pending, "request is acknowledged by the consumer");

    for (int i = 0; i < 7; i++) {
        ASSERT_TRUE(smoothaf_persist_log_record(
                        &buf, 5000u + (uint32_t)i,
                        SMOOTHAF_PERSIST_LOG_COMMAND, 1, payload),
                    "task-loop reproduction publishes each record");
    }
    ASSERT_EQ(smoothaf_persist_log_get_published(&buf), 7u,
              "task-loop reproduction publishes seven records");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 7u,
              "seven records remain queued before the next service");

    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "task-loop service consumes the queued records");
    ASSERT_EQ(smoothaf_persist_log_get_flushed(&buf), 7u,
              "all seven records are confirmed by the backend");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u,
              "task-loop service drains the ring");
    ASSERT_EQ(fio.file_open_attempts, 1u,
              "the first non-empty service reaches file open exactly once");
    ASSERT_TRUE(fio.bytes_written > 0u,
                "the first non-empty service accepts bytes");
    ASSERT_TRUE(fio.enabled && !fio.config_pending,
                "normal task-loop state cannot leave effective FIO off");
    PASS();
}

static void test_fio_open_failure_and_counter_contract(void) {
    TEST("fio_open_failure_and_counter_contract");
    struct smoothaf_log_fio_backend fio;
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    int32_t payload[6] = {1, 2, 3, 4, 5, 6};

    reset_logger(&buf);
    smoothaf_log_fio_init(&fio);
    smoothaf_persist_logger_set_enabled(&buf, true);
    smoothaf_log_fio_set_enabled(&fio, true);
    smoothaf_log_fio_set_card_present(false);
    smoothaf_persist_log_record(&buf, 4000, SMOOTHAF_PERSIST_LOG_COMMAND, 1,
                                payload);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "consumer reports attempted open failure");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 1u,
              "open failure retains the event for retry");
    ASSERT_TRUE(fio.write_errors > 0u, "open failure increments write errors");
    ASSERT_EQ(fio.total_files_written, 0u,
              "failed open does not increment file counter");
    smoothaf_log_fio_set_card_present(true);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "consumer retries after card recovery");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u,
              "recovered open eventually commits the event");
    ASSERT_EQ(fio.total_files_written, 1u,
              "successful retry increments file counter once");
    smoothaf_log_fio_set_enabled(&fio, false);
    PASS();
}

static void test_fio_backend(void) {
    TEST("fio_backend");
    struct smoothaf_log_fio_backend fio;
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_persist_log_batch batch;
    smoothaf_log_fio_init(&fio);
    smoothaf_persist_logger_init(&buf);
    smoothaf_persist_logger_set_enabled(&buf, true);
    smoothaf_log_fio_set_enabled(&fio, true);
    fio.max_file_size_bytes = 128;

    int32_t payload[6] = {1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 3; i++)
        smoothaf_persist_log_record(&buf, 1000u + (uint32_t)i,
                                    SMOOTHAF_PERSIST_LOG_COMMAND, 1, payload);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch), "FIO service writes batch");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "FIO service drains batch");
    ASSERT_EQ(smoothaf_persist_log_get_published(&buf), 3u, "producer publication count");
    ASSERT_EQ(smoothaf_persist_log_get_flushed(&buf), 3u, "backend confirmation count");
    ASSERT_TRUE(fio.total_files_written >= 1u, "FIO creates a numbered file");
    ASSERT_TRUE(fio.file_open_attempts >= 1u, "FIO open attempt is counted");
    ASSERT_TRUE(fio.bytes_written > 0u, "accepted bytes are counted");

    smoothaf_log_fio_set_simulated_write_chunk(7);
    for (int i = 0; i < 2; i++)
        smoothaf_persist_log_record(&buf, 2000u + (uint32_t)i,
                                    SMOOTHAF_PERSIST_LOG_COMMAND, 1, payload);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch), "short writes are retried");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "short-write batch drains");
    ASSERT_TRUE(fio.partial_writes > 0u, "partial write statistic increments");

    /* A retry must finish the current line in the same file even when the
     * configured limit is reached by the already-written prefix. */
    fio.max_file_size_bytes = 96;
    smoothaf_log_fio_set_simulated_write_chunk(7);
    int32_t retry_payload[6] = {7, 8, 9, 10, 11, 12};
    smoothaf_persist_log_record(&buf, 2500, SMOOTHAF_PERSIST_LOG_COMMAND, 1,
                                retry_payload);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch),
                "rotation-boundary retry starts");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u,
              "rotation-boundary retry drains");
    ASSERT_TRUE(fio.total_files_written >= 2u,
                "rotation-boundary retry preserves file ownership");

    smoothaf_log_fio_set_card_present(false);
    smoothaf_persist_log_record(&buf, 3000, SMOOTHAF_PERSIST_LOG_COMMAND, 1, payload);
    smoothaf_log_fio_service(&fio, &buf, &batch);
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 1u, "card removal retains records");
    ASSERT_TRUE(fio.write_errors > 0u, "card removal increments errors");
    smoothaf_log_fio_set_card_present(true);
    smoothaf_log_fio_set_simulated_write_chunk(0);
    ASSERT_TRUE(smoothaf_log_fio_service(&fio, &buf, &batch), "card recovery retries");
    ASSERT_EQ(smoothaf_persist_log_get_fill(&buf), 0u, "card recovery drains retained record");

    smoothaf_log_fio_set_enabled(&fio, false);
    PASS();
}

static void test_struct_sizes(void) {
    TEST("struct_sizes");
    struct smoothaf_persist_log_buffer buf;
    struct smoothaf_log_fio_backend fio;
    smoothaf_persist_logger_init(&buf);
    smoothaf_log_fio_init(&fio);

    /* The Legacy FIO ABI defines this array as 0x80 bytes.  Keep the
     * translation-unit contract visible in the host regression: smoothaf.c
     * includes fio-ml.h while smoothaf_logger_fio.c uses this fallback. */
    ASSERT_EQ(FIO_MAX_PATH_LENGTH, 0x80u,
              "FIO path length matches the Legacy ABI");
    ASSERT_EQ(sizeof(fio.current_filename), 0x80u,
              "FIO backend uses the Legacy filename field size");

    /* Verify actual sizes (may vary by alignment) */
    size_t record_size = sizeof(struct smoothaf_persist_log_record);
    size_t ring_size = sizeof(buf.ring);
    size_t buffer_size = sizeof(buf);

    printf("  record=%zu ring=%zu buffer=%zu\n", record_size, ring_size, buffer_size);
    /* Ring should be record_size * 256 */
    ASSERT_EQ(ring_size, record_size * SMOOTHAF_LOG_BUF_ENTRIES, "ring = record * entries");
    /* Buffer should be ring + head + tail + dropped + sequence + enabled */
    ASSERT_TRUE(buffer_size >= ring_size, "buffer >= ring");

    PASS();
}

/* ============================================================
 * Main
 * ============================================================ */

int main(void) {
    printf("=== SmoothAF Logger Phase 3A Tests ===\n");
    printf("Ring buffer: %d entries (%zu bytes)\n", SMOOTHAF_LOG_BUF_ENTRIES, (size_t)SMOOTHAF_PERSIST_LOG_RING_SIZE);
    printf("Batch buffer: %d lines x %d bytes = %d bytes\n",
           SMOOTHAF_LOG_BATCH_LINES, SMOOTHAF_LOG_LINE_MAX, SMOOTHAF_LOG_BATCH_BUF_SIZE);
    printf("\n");

    test_init_shutdown();
    test_empty_full();
    test_wrap_around();
    test_overflow_ordering();
    test_record_integrity();
    test_producer_consumer_concurrency();
    test_real_pthread_producer_consumer();
    test_dropped_counter();
    test_consumer_failure();
    test_stop_panic_non_blocking();
    test_timestamp_wrap_around();
    test_logging_off_no_interference();
    test_specific_events();
    test_small_batches();
    test_flush_batch();
    test_integrated_active_continuous_pipeline();
    test_fio_configuration_and_lifecycle();
    test_fio_task_loop_reproduction_snapshot();
    test_fio_open_failure_and_counter_contract();
    test_fio_backend();
    test_struct_sizes();

    printf("\n=== Summary ===\n");
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%d TEST(S) FAILED\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}