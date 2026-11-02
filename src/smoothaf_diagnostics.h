#ifndef SMOOTHAF_DIAGNOSTICS_H
#define SMOOTHAF_DIAGNOSTICS_H

#include <stdint.h>

/* One bounded in-RAM status record. No history, logging, or filesystem state. */
struct smoothaf_telemetry
{
    int32_t score;
    uint32_t duration_ms;
    int32_t direction;
    int32_t step;
    int32_t state;
    int32_t error;
    int32_t reason;
};

static inline void smoothaf_telemetry_init(struct smoothaf_telemetry *telemetry)
{
    telemetry->score = 0;
    telemetry->duration_ms = 0;
    telemetry->direction = 0;
    telemetry->step = 0;
    telemetry->state = 0;
    telemetry->error = 0;
    telemetry->reason = 0;
}

#endif
