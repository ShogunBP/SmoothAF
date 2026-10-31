/*
 * SmoothAF Canon AF Assist boundary.
 *
 * This host-testable state machine deliberately has no Magic Lantern or lens
 * dependencies.  The unavailable adapter is the only implementation until a
 * C-level Canon AF API has a verified ABI, symbol and cancellation contract.
 */

#ifndef SMOOTHAF_AF_ASSIST_H
#define SMOOTHAF_AF_ASSIST_H

#include <stdint.h>

#define SMOOTHAF_AF_ASSIST_REQUEST_NONE 0U

enum smoothaf_af_assist_state
{
    SMOOTHAF_AF_ASSIST_UNAVAILABLE = 0,
    SMOOTHAF_AF_ASSIST_IDLE,
    SMOOTHAF_AF_ASSIST_READY_READONLY,
    SMOOTHAF_AF_ASSIST_ARMED,
    SMOOTHAF_AF_ASSIST_REQUESTED,
    SMOOTHAF_AF_ASSIST_IN_FLIGHT,
    SMOOTHAF_AF_ASSIST_COMPLETED,
    SMOOTHAF_AF_ASSIST_CANCELLED,
    SMOOTHAF_AF_ASSIST_TIMEOUT,
    SMOOTHAF_AF_ASSIST_FAULT,
};

enum smoothaf_af_assist_result
{
    SMOOTHAF_AF_ASSIST_RESULT_UNAVAILABLE = 0,
    SMOOTHAF_AF_ASSIST_RESULT_ACCEPTED,
    SMOOTHAF_AF_ASSIST_RESULT_COMPLETED,
    SMOOTHAF_AF_ASSIST_RESULT_CANCELLED,
    SMOOTHAF_AF_ASSIST_RESULT_TIMEOUT,
    SMOOTHAF_AF_ASSIST_RESULT_FAILED,
};

enum smoothaf_af_assist_block
{
    SMOOTHAF_AF_ASSIST_BLOCK_NONE = 0,
    SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED,
    SMOOTHAF_AF_ASSIST_BLOCK_DISABLED,
    SMOOTHAF_AF_ASSIST_BLOCK_ACTIVE,
    SMOOTHAF_AF_ASSIST_BLOCK_LIVEVIEW,
    SMOOTHAF_AF_ASSIST_BLOCK_MOVIE,
    SMOOTHAF_AF_ASSIST_BLOCK_RECORDING,
    SMOOTHAF_AF_ASSIST_BLOCK_MANUAL_FOCUS,
    SMOOTHAF_AF_ASSIST_BLOCK_CONTINUOUS_AF,
    SMOOTHAF_AF_ASSIST_BLOCK_CONFLICT,
    SMOOTHAF_AF_ASSIST_BLOCK_SEMAPHORE,
};

struct smoothaf_af_assist
{
    enum smoothaf_af_assist_state state;
    enum smoothaf_af_assist_block block;
    uint32_t request_id;
    int cleanup_required;
};

static inline void smoothaf_af_assist_init(struct smoothaf_af_assist *assist)
{
    assist->state = SMOOTHAF_AF_ASSIST_UNAVAILABLE;
    assist->block = SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED;
    assist->request_id = SMOOTHAF_AF_ASSIST_REQUEST_NONE;
    assist->cleanup_required = 0;
}

static inline const char *smoothaf_af_assist_state_name(
        enum smoothaf_af_assist_state state)
{
    switch (state)
    {
        case SMOOTHAF_AF_ASSIST_IDLE:          return "IDLE";
        case SMOOTHAF_AF_ASSIST_READY_READONLY: return "READY_READONLY";
        case SMOOTHAF_AF_ASSIST_ARMED:         return "ARMED";
        case SMOOTHAF_AF_ASSIST_REQUESTED:     return "REQUESTED";
        case SMOOTHAF_AF_ASSIST_IN_FLIGHT:    return "IN_FLIGHT";
        case SMOOTHAF_AF_ASSIST_COMPLETED:    return "COMPLETED";
        case SMOOTHAF_AF_ASSIST_CANCELLED:    return "CANCELLED";
        case SMOOTHAF_AF_ASSIST_TIMEOUT:      return "TIMEOUT";
        case SMOOTHAF_AF_ASSIST_FAULT:        return "FAULT";
        default:                              return "UNAVAILABLE";
    }
}

static inline const char *smoothaf_af_assist_block_name(
        enum smoothaf_af_assist_block block)
{
    switch (block)
    {
        case SMOOTHAF_AF_ASSIST_BLOCK_DISABLED:      return "Enable is OFF";
        case SMOOTHAF_AF_ASSIST_BLOCK_ACTIVE:        return "AF Assist is active";
        case SMOOTHAF_AF_ASSIST_BLOCK_LIVEVIEW:     return "LiveView is unavailable";
        case SMOOTHAF_AF_ASSIST_BLOCK_MOVIE:        return "Movie mode";
        case SMOOTHAF_AF_ASSIST_BLOCK_RECORDING:    return "Recording";
        case SMOOTHAF_AF_ASSIST_BLOCK_MANUAL_FOCUS: return "Manual focus";
        case SMOOTHAF_AF_ASSIST_BLOCK_CONTINUOUS_AF:return "Continuous AF";
        case SMOOTHAF_AF_ASSIST_BLOCK_CONFLICT:     return "Focus controller conflict";
        case SMOOTHAF_AF_ASSIST_BLOCK_SEMAPHORE:    return "Semaphore unavailable";
        default:                                    return "No safe Canon C API";
    }
}

/* The unavailable adapter cannot accept a request and has no cleanup work. */
static inline enum smoothaf_af_assist_result smoothaf_af_assist_request(
        struct smoothaf_af_assist *assist, enum smoothaf_af_assist_block block)
{
    assist->block = block == SMOOTHAF_AF_ASSIST_BLOCK_NONE
                  ? SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED : block;
    assist->state = SMOOTHAF_AF_ASSIST_UNAVAILABLE;
    assist->cleanup_required = 0;
    return SMOOTHAF_AF_ASSIST_RESULT_UNAVAILABLE;
}

static inline enum smoothaf_af_assist_result smoothaf_af_assist_cancel(
        struct smoothaf_af_assist *assist)
{
    assist->state = SMOOTHAF_AF_ASSIST_CANCELLED;
    assist->block = SMOOTHAF_AF_ASSIST_BLOCK_UNSUPPORTED;
    assist->cleanup_required = 0;
    return SMOOTHAF_AF_ASSIST_RESULT_CANCELLED;
}

static inline void smoothaf_af_assist_reset(struct smoothaf_af_assist *assist)
{
    smoothaf_af_assist_init(assist);
}

#endif
