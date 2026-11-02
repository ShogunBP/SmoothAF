#ifndef SMOOTHAF_LOG_EXPORT_H
#define SMOOTHAF_LOG_EXPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include "smoothaf_diagnostics.h"

#define SMOOTHAF_LOG_EXPORT_VERSION "1"
#define SMOOTHAF_LOG_EXPORT_PATH_PATTERN "ML/LOGS/SMOOTHAF-%03d.LOG"
#define SMOOTHAF_LOG_EXPORT_LINE_MAX 192
#define SMOOTHAF_LOG_EXPORT_NAME_LIMIT 1000

typedef int (*smoothaf_log_export_write_fn)(void *context,
                                             const char *data,
                                             size_t length);

struct smoothaf_log_export_metadata
{
    const char *mode;
    uint32_t configured_interval_ms;
    uint32_t observed_interval_ms;
    uint32_t session_started_ms;
    uint32_t session_finished_ms;
    size_t dropped;
};

struct smoothaf_log_export_result
{
    size_t events;
    size_t bytes;
    int truncated;
};

static int smoothaf_log_export_emit(smoothaf_log_export_write_fn write,
                                           void *context,
                                           size_t *bytes,
                                           const char *format, ...)
{
    char line[SMOOTHAF_LOG_EXPORT_LINE_MAX];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(line))
        return -1;
    if (write(context, line, (size_t)length) != length)
        return -2;
    *bytes += (size_t)length;
    return 0;
}

static inline int smoothaf_log_export_snapshot(
    const struct smoothaf_log_entry *entries, size_t count,
    const struct smoothaf_log_export_metadata *metadata,
    smoothaf_log_export_write_fn write, void *context,
    struct smoothaf_log_export_result *result)
{
    size_t i;
    int rc;

    result->events = 0;
    result->bytes = 0;
    result->truncated = 0;
    rc = smoothaf_log_export_emit(write, context, &result->bytes,
        "SmoothAF Version: 0.8.0\nLog Format: %s\nMode: %s\n",
        SMOOTHAF_LOG_EXPORT_VERSION,
        metadata->mode ? metadata->mode : "UNKNOWN");
    if (rc) return rc;
    rc = smoothaf_log_export_emit(write, context, &result->bytes,
        "Configured Interval: %u ms\nObserved Interval: %u ms\n"
        "Session Start: %u ms\nSession End: %u ms\nEvents Dropped: %u\n",
        (unsigned)metadata->configured_interval_ms,
        (unsigned)metadata->observed_interval_ms,
        (unsigned)metadata->session_started_ms,
        (unsigned)metadata->session_finished_ms,
        (unsigned)metadata->dropped);
    if (rc) return rc;
    rc = smoothaf_log_export_emit(write, context, &result->bytes,
        "Timestamp(ms) Type Value1 Value2 Value3 Value4 Value5\n");
    if (rc) return rc;

    for (i = 0; i < count; i++)
    {
        const struct smoothaf_log_entry *entry = &entries[i];
        rc = smoothaf_log_export_emit(write, context, &result->bytes,
            "%u %s %d %d %d %d %d\n",
            (unsigned)entry->timestamp_ms,
            smoothaf_log_type_name(entry->type),
            (int)entry->value1, (int)entry->value2, (int)entry->value3,
            (int)entry->value4, (int)entry->value5);
        if (rc) return rc;
        result->events++;
    }
    return 0;
}

#endif
