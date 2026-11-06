#include "smoothaf_observability.h"

#include <string.h>

static uint32_t obs_u32_delta(uint32_t before, uint32_t after,
                              uint8_t *wrapped, uint8_t *ambiguous)
{
    uint32_t delta = after - before;
    if (wrapped) *wrapped = after < before ? 1u : 0u;
    if (ambiguous) *ambiguous = delta > SMOOTHAF_OBS_UINT32_HALF ? 1u : 0u;
    return delta;
}

static int obs_geometry_valid(const struct smoothaf_observability_snapshot *snapshot)
{
    return snapshot->width > 0 && snapshot->height > 0 &&
           snapshot->pitch >= snapshot->width * 2;
}

static int obs_logical_id_valid(uint16_t logical_id)
{
    return logical_id != 0u && logical_id != SMOOTHAF_OBS_UINT16_MAX;
}

static int obs_profile_fields(enum smoothaf_observability_profile profile,
                              uint32_t *fields)
{
    if (!fields) return 0;
    if (profile == SMOOTHAF_OBS_PROFILE_CORE) {
        *fields = SMOOTHAF_OBS_CORE_FIELDS;
        return 1;
    }
    if (profile == SMOOTHAF_OBS_PROFILE_EXTENDED) {
        *fields = SMOOTHAF_OBS_EXTENDED_FIELDS;
        return 1;
    }
    return 0;
}

void smoothaf_observability_snapshot_init(
    struct smoothaf_observability_snapshot *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->yuv_buffer_address = SMOOTHAF_OBS_UINT32_SENTINEL;
    snapshot->edmac_write_address = SMOOTHAF_OBS_UINT32_SENTINEL;
    snapshot->display_buffer_address = SMOOTHAF_OBS_UINT32_SENTINEL;
    snapshot->mvr_frame_number = SMOOTHAF_OBS_UINT32_SENTINEL;
    snapshot->available_mask = 0u;
    snapshot->environment_valid = 1u;
}

uint32_t smoothaf_observability_pack_exposure(
    const struct smoothaf_observability_exposure *exposure)
{
    if (!exposure) return SMOOTHAF_OBS_UINT32_SENTINEL;
    return (uint32_t)exposure->aperture |
           ((uint32_t)exposure->shutter << 8) |
           ((uint32_t)exposure->iso << 16) |
           ((uint32_t)exposure->iso_auto << 24);
}

int smoothaf_observability_classify(
    const struct smoothaf_observability_snapshot *before,
    const struct smoothaf_observability_snapshot *after,
    enum smoothaf_observability_profile profile,
    struct smoothaf_observability_result *result)
{
    uint32_t profile_fields;
    uint32_t available;
    uint32_t missing;
    uint32_t changed = 0u;
    uint32_t invalid = 0u;
    uint32_t invalid_reasons = 0u;
    uint32_t timestamp_delta;
    uint8_t timestamp_wrapped = 0u;
    uint8_t timestamp_ambiguous = 0u;
    uint8_t mvr_wrapped = 0u;
    uint8_t mvr_non_monotonic = 0u;

    if (!before || !after || !result ||
        !obs_profile_fields(profile, &profile_fields)) return 0;

    memset(result, 0, sizeof(*result));
    result->profile = profile;
    available = before->available_mask & after->available_mask & profile_fields;
    result->available_mask = available;
    missing = profile_fields & ~available;
    result->missing_mask = missing;
    result->missing_required_mask = missing & SMOOTHAF_OBS_CORE_FIELDS;

#define OBS_COMPARE(field, left, right) \
    do { if ((field & profile_fields) && (field & available) && \
             ((left) != (right))) changed |= (field); } while (0)
    OBS_COMPARE(SMOOTHAF_OBS_FIELD_YUV_BUFFER,
                before->yuv_buffer_address, after->yuv_buffer_address);
    if ((SMOOTHAF_OBS_FIELD_GEOMETRY & profile_fields) &&
        (SMOOTHAF_OBS_FIELD_GEOMETRY & available) &&
        (before->width != after->width || before->pitch != after->pitch ||
         before->height != after->height))
        changed |= SMOOTHAF_OBS_FIELD_GEOMETRY;
    OBS_COMPARE(SMOOTHAF_OBS_FIELD_LIVEVIEW, before->liveview, after->liveview);
    OBS_COMPARE(SMOOTHAF_OBS_FIELD_MOVIE_MODE,
                before->movie_mode, after->movie_mode);
    OBS_COMPARE(SMOOTHAF_OBS_FIELD_RECORDING,
                before->recording_class, after->recording_class);
    if (profile == SMOOTHAF_OBS_PROFILE_EXTENDED) {
        OBS_COMPARE(SMOOTHAF_OBS_FIELD_EDMAC,
                    before->edmac_write_address, after->edmac_write_address);
        OBS_COMPARE(SMOOTHAF_OBS_FIELD_DISPLAY,
                    before->display_buffer_address, after->display_buffer_address);
        if ((SMOOTHAF_OBS_FIELD_EXPOSURE & available) &&
            memcmp(&before->exposure, &after->exposure,
                   sizeof(before->exposure)) != 0)
            changed |= SMOOTHAF_OBS_FIELD_EXPOSURE;
        OBS_COMPARE(SMOOTHAF_OBS_FIELD_MVR,
                    before->mvr_frame_number, after->mvr_frame_number);
    }
#undef OBS_COMPARE

    timestamp_delta = obs_u32_delta(before->timestamp_ms, after->timestamp_ms,
                                    &timestamp_wrapped, &timestamp_ambiguous);
    if ((SMOOTHAF_OBS_FIELD_TIMESTAMP & available) &&
        timestamp_ambiguous) {
        invalid |= SMOOTHAF_OBS_FIELD_TIMESTAMP;
        invalid_reasons |= SMOOTHAF_OBS_REASON_TIMESTAMP;
    }
    if (profile == SMOOTHAF_OBS_PROFILE_EXTENDED &&
        (SMOOTHAF_OBS_FIELD_MVR & available)) {
        uint8_t ignored_ambiguous = 0u;
        (void)obs_u32_delta(before->mvr_frame_number, after->mvr_frame_number,
                            &mvr_wrapped, &ignored_ambiguous);
        if (ignored_ambiguous) {
            mvr_non_monotonic = 1u;
            invalid |= SMOOTHAF_OBS_FIELD_MVR;
            invalid_reasons |= SMOOTHAF_OBS_REASON_MVR_COUNTER;
        }
    }
    if (!before->environment_valid || !after->environment_valid) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_ENVIRONMENT;
    }
    if ((SMOOTHAF_OBS_FIELD_LIVEVIEW & available) &&
        (!before->liveview || !after->liveview)) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_ENVIRONMENT;
        invalid |= SMOOTHAF_OBS_FIELD_LIVEVIEW;
    }
    if ((SMOOTHAF_OBS_FIELD_MOVIE_MODE & available) &&
        (!before->movie_mode || !after->movie_mode)) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_ENVIRONMENT;
        invalid |= SMOOTHAF_OBS_FIELD_MOVIE_MODE;
    }
    if ((SMOOTHAF_OBS_FIELD_RECORDING & available) &&
        before->recording_class != after->recording_class) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_RECORDING;
        invalid |= SMOOTHAF_OBS_FIELD_RECORDING;
    }
    if ((SMOOTHAF_OBS_FIELD_GEOMETRY & available) &&
        (!obs_geometry_valid(before) || !obs_geometry_valid(after) ||
         before->width != after->width || before->pitch != after->pitch ||
         before->height != after->height)) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_GEOMETRY;
        invalid |= SMOOTHAF_OBS_FIELD_GEOMETRY;
    }
    if (result->missing_required_mask) {
        invalid_reasons |= SMOOTHAF_OBS_REASON_REQUIRED;
        invalid |= result->missing_required_mask;
    }
    if (before->invalid_reasons || after->invalid_reasons) {
        invalid_reasons |= before->invalid_reasons | after->invalid_reasons;
        invalid |= result->missing_mask;
    }

    result->changed_mask = changed;
    result->invalid_mask = invalid;
    result->invalid_reason_mask = invalid_reasons;
    result->timestamp_delta_ms = timestamp_delta;
    result->timestamp_wrapped = timestamp_wrapped;
    result->timestamp_ambiguous = timestamp_ambiguous;
    result->mvr_wrapped = mvr_wrapped;
    result->mvr_non_monotonic = mvr_non_monotonic;
    if (invalid_reasons)
        result->status = SMOOTHAF_OBS_INVALID_ENVIRONMENT;
    else if (changed)
        result->status = SMOOTHAF_OBS_CHANGED_DURING_MEASUREMENT;
    else if (missing)
        result->status = SMOOTHAF_OBS_INSUFFICIENT_OBSERVABILITY;
    else
        result->status = SMOOTHAF_OBS_STABLE_METADATA;
    return 1;
}

void smoothaf_observability_controller_init(
    struct smoothaf_observability_controller *controller,
    uint32_t minimum_window_ms)
{
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    controller->next_logical_id = 1u;
    controller->minimum_window_ms = minimum_window_ms;
    controller->identity_namespace = SMOOTHAF_OBS_IDENTITY_NONE;
    controller->identity_generation = 0u;
}

void smoothaf_observability_controller_set_identity(
    struct smoothaf_observability_controller *controller,
    uint8_t identity_namespace,
    uint32_t identity_generation)
{
    if (!controller || identity_namespace > SMOOTHAF_OBS_IDENTITY_MANUAL) return;
    /* A failed best-effort flush belongs to the old context.  It cannot be
     * carried into a new namespace without risking a cross-context pair, so
     * account it as an incomplete terminal drop before switching. */
    if (controller->has_pending) {
        controller->dropped_events += 2u;
        controller->incomplete_pairs++;
        controller->has_pending = 0u;
    }
    controller->identity_namespace = identity_namespace;
    controller->identity_generation = identity_generation;
    controller->has_published = 0u;
}

void smoothaf_observability_event_set_identity(
    struct smoothaf_observability_event *event,
    uint8_t identity_namespace,
    uint32_t identity_generation)
{
    if (!event || identity_namespace > SMOOTHAF_OBS_IDENTITY_MANUAL) return;
    event->identity_namespace = identity_namespace;
    event->identity_generation = identity_generation;
}

uint16_t smoothaf_observability_next_logical_id(
    struct smoothaf_observability_controller *controller)
{
    uint16_t result;
    if (!controller) return 0u;
    result = controller->next_logical_id;
    if (!obs_logical_id_valid(result)) result = 1u;
    controller->next_logical_id = result >= (SMOOTHAF_OBS_UINT16_MAX - 1u)
                                ? 1u : (uint16_t)(result + 1u);
    return result;
}

int smoothaf_observability_make_event(
    struct smoothaf_observability_event *event,
    const struct smoothaf_observability_snapshot *snapshot,
    const struct smoothaf_observability_result *result,
    uint16_t episode_id,
    uint32_t sequence,
    uint16_t logical_id,
    enum smoothaf_observability_phase phase)
{
    uint32_t available;
    if (!event || !snapshot || !result || !obs_logical_id_valid(logical_id) ||
        phase > SMOOTHAF_OBS_PHASE_AFTER) return 0;
    available = result->available_mask;
    memset(event, 0, sizeof(*event));
    event->timestamp_ms = snapshot->timestamp_ms;
    event->event_type = SMOOTHAF_OBS_EVENT_TYPE;
    event->episode_id = episode_id;
    event->sequence = sequence;
    event->identity_namespace = SMOOTHAF_OBS_IDENTITY_NONE;
    event->identity_generation = 0u;
    event->logical_id = logical_id;
    event->phase = phase;
    event->status = result->status;
    event->profile = result->profile;
    if (available & SMOOTHAF_OBS_FIELD_YUV_BUFFER)
        event->flags |= SMOOTHAF_OBS_FLAG_YUV_AVAILABLE;
    if (available & SMOOTHAF_OBS_FIELD_GEOMETRY)
        event->flags |= SMOOTHAF_OBS_FLAG_GEOMETRY_AVAILABLE;
    if (available & SMOOTHAF_OBS_FIELD_EDMAC)
        event->flags |= SMOOTHAF_OBS_FLAG_EDMAC_AVAILABLE;
    if (available & SMOOTHAF_OBS_FIELD_DISPLAY)
        event->flags |= SMOOTHAF_OBS_FLAG_DISPLAY_AVAILABLE;
    if (available & SMOOTHAF_OBS_FIELD_EXPOSURE)
        event->flags |= SMOOTHAF_OBS_FLAG_EXPOSURE_AVAILABLE;
    if (available & SMOOTHAF_OBS_FIELD_MVR)
        event->flags |= SMOOTHAF_OBS_FLAG_MVR_AVAILABLE;
    if (result->timestamp_wrapped)
        event->flags |= SMOOTHAF_OBS_FLAG_TIMESTAMP_WRAPPED;
    if (result->mvr_wrapped)
        event->flags |= SMOOTHAF_OBS_FLAG_MVR_WRAPPED;
    event->yuv_buffer_address = (event->flags & SMOOTHAF_OBS_FLAG_YUV_AVAILABLE)
                              ? snapshot->yuv_buffer_address : SMOOTHAF_OBS_UINT32_SENTINEL;
    event->edmac_write_address = (event->flags & SMOOTHAF_OBS_FLAG_EDMAC_AVAILABLE)
                               ? snapshot->edmac_write_address : SMOOTHAF_OBS_UINT32_SENTINEL;
    event->display_buffer_address = (event->flags & SMOOTHAF_OBS_FLAG_DISPLAY_AVAILABLE)
                                  ? snapshot->display_buffer_address : SMOOTHAF_OBS_UINT32_SENTINEL;
    event->mvr_frame_number = (event->flags & SMOOTHAF_OBS_FLAG_MVR_AVAILABLE)
                            ? snapshot->mvr_frame_number : SMOOTHAF_OBS_UINT32_SENTINEL;
    event->packed_exposure = (event->flags & SMOOTHAF_OBS_FLAG_EXPOSURE_AVAILABLE)
                           ? smoothaf_observability_pack_exposure(&snapshot->exposure)
                           : SMOOTHAF_OBS_UINT32_SENTINEL;
    return 1;
}

static int obs_publish_pair(struct smoothaf_observability_controller *controller,
                            const struct smoothaf_observability_event *before,
                            const struct smoothaf_observability_event *after,
                            smoothaf_observability_publish_fn publish,
                            void *context)
{
    int before_ok;
    int after_ok;
    if (!controller || !before || !after || !publish) return 0;
    before_ok = publish(context, before) ? 1 : 0;
    after_ok = publish(context, after) ? 1 : 0;
    controller->published_events += (uint32_t)(before_ok + after_ok);
    controller->dropped_events += (uint32_t)(2 - before_ok - after_ok);
    if (before_ok != after_ok) controller->incomplete_pairs++;
    return before_ok && after_ok;
}

static int obs_status_valid(enum smoothaf_observability_status status)
{
    return status <= SMOOTHAF_OBS_INVALID_ENVIRONMENT;
}

int smoothaf_observability_submit_pair(
    struct smoothaf_observability_controller *controller,
    const struct smoothaf_observability_event *before,
    const struct smoothaf_observability_event *after,
    smoothaf_observability_publish_fn publish,
    void *context)
{
    uint32_t delta;
    uint8_t wrapped;
    uint8_t ambiguous;
    int due;
    if (!controller || !before || !after ||
        before->phase != SMOOTHAF_OBS_PHASE_BEFORE ||
        after->phase != SMOOTHAF_OBS_PHASE_AFTER ||
        before->episode_id != after->episode_id ||
        before->logical_id != after->logical_id ||
        before->identity_namespace != after->identity_namespace ||
        before->identity_generation != after->identity_generation ||
        (controller->identity_namespace != SMOOTHAF_OBS_IDENTITY_NONE &&
         (before->identity_namespace != controller->identity_namespace ||
          before->identity_generation != controller->identity_generation)) ||
        !obs_logical_id_valid(before->logical_id) ||
        !obs_logical_id_valid(after->logical_id) ||
        !obs_status_valid(before->status) || !obs_status_valid(after->status)) {
        if (controller) controller->incomplete_pairs++;
        return 0;
    }
    if (!controller->has_published) {
        due = 1;
    } else {
        delta = obs_u32_delta(controller->last_timestamp_ms,
                              after->timestamp_ms, &wrapped, &ambiguous);
        (void)wrapped;
        due = !ambiguous && delta >= controller->minimum_window_ms;
        if (after->status != controller->last_status && due) due = 1;
    }
    if (controller->has_published && after->status != controller->last_status &&
        !due) {
        controller->pending_before = *before;
        controller->pending_after = *after;
        controller->has_pending = 1u;
        controller->suppressed_observations++;
        return 0;
    }
    if (!due) {
        controller->pending_before = *before;
        controller->pending_after = *after;
        controller->has_pending = 1u;
        controller->suppressed_observations++;
        return 0;
    }
    {
        int published = obs_publish_pair(controller, before, after, publish, context);
        if (published) {
            controller->last_timestamp_ms = after->timestamp_ms;
            controller->last_status = after->status;
            controller->has_published = 1u;
            controller->has_pending = 0u;
        } else {
            /* Keep the rejected pair as the retry candidate, but do not let a
             * partial attempt masquerade as a complete publication. */
            controller->pending_before = *before;
            controller->pending_after = *after;
            controller->has_pending = 1u;
        }
        return published;
    }
}

int smoothaf_observability_flush_pending(
    struct smoothaf_observability_controller *controller,
    smoothaf_observability_publish_fn publish,
    void *context)
{
    int result;
    if (!controller || !controller->has_pending) return 0;
    result = obs_publish_pair(controller, &controller->pending_before,
                              &controller->pending_after, publish, context);
    if (result) {
        controller->last_timestamp_ms = controller->pending_after.timestamp_ms;
        controller->last_status = controller->pending_after.status;
        controller->has_published = 1u;
        controller->has_pending = 0u;
    }
    return result;
}

static void obs_put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xFFu);
    output[1] = (uint8_t)(value >> 8);
}

static void obs_put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & 0xFFu);
    output[1] = (uint8_t)((value >> 8) & 0xFFu);
    output[2] = (uint8_t)((value >> 16) & 0xFFu);
    output[3] = (uint8_t)(value >> 24);
}

int smoothaf_observability_event_payload(
    const struct smoothaf_observability_event *event,
    int32_t payload[SMOOTHAF_OBS_PAYLOAD_SLOTS])
{
    uint32_t control;
    uint32_t flags;
    if (!event || !payload || event->event_type != SMOOTHAF_OBS_EVENT_TYPE ||
        event->identity_namespace > SMOOTHAF_OBS_IDENTITY_MANUAL ||
        !obs_logical_id_valid(event->logical_id) ||
        event->phase > SMOOTHAF_OBS_PHASE_AFTER ||
        event->status > SMOOTHAF_OBS_INVALID_ENVIRONMENT ||
        event->profile > SMOOTHAF_OBS_PROFILE_EXTENDED ||
        (event->flags & ~SMOOTHAF_OBS_FLAGS_MASK) != 0u)
        return 0;
    flags = event->flags;
    if (event->profile == SMOOTHAF_OBS_PROFILE_CORE &&
        (flags & SMOOTHAF_OBS_EXTENDED_FLAGS)) return 0;
    if (event->profile == SMOOTHAF_OBS_PROFILE_CORE &&
        !(flags & SMOOTHAF_OBS_FLAG_YUV_AVAILABLE)) return 0;
    if ((flags & SMOOTHAF_OBS_FLAG_MVR_WRAPPED) &&
        !(flags & SMOOTHAF_OBS_FLAG_MVR_AVAILABLE)) return 0;
    if (!!(flags & SMOOTHAF_OBS_FLAG_YUV_AVAILABLE) !=
        (event->yuv_buffer_address != SMOOTHAF_OBS_UINT32_SENTINEL)) return 0;
    if (!!(flags & SMOOTHAF_OBS_FLAG_EDMAC_AVAILABLE) !=
        (event->edmac_write_address != SMOOTHAF_OBS_UINT32_SENTINEL)) return 0;
    if (!!(flags & SMOOTHAF_OBS_FLAG_DISPLAY_AVAILABLE) !=
        (event->display_buffer_address != SMOOTHAF_OBS_UINT32_SENTINEL)) return 0;
    if (!!(flags & SMOOTHAF_OBS_FLAG_MVR_AVAILABLE) !=
        (event->mvr_frame_number != SMOOTHAF_OBS_UINT32_SENTINEL)) return 0;
    if (!!(flags & SMOOTHAF_OBS_FLAG_EXPOSURE_AVAILABLE) !=
        (event->packed_exposure != SMOOTHAF_OBS_UINT32_SENTINEL)) return 0;
    control = (SMOOTHAF_OBS_SCHEMA_VERSION << SMOOTHAF_OBS_VERSION_SHIFT) |
              ((uint32_t)event->phase << SMOOTHAF_OBS_PHASE_SHIFT) |
              ((uint32_t)event->status << SMOOTHAF_OBS_STATUS_SHIFT) |
              ((uint32_t)event->profile << SMOOTHAF_OBS_PROFILE_SHIFT) |
              (flags << SMOOTHAF_OBS_FLAGS_SHIFT) |
              ((uint32_t)event->logical_id << SMOOTHAF_OBS_LOGICAL_ID_SHIFT);
    payload[0] = (int32_t)control;
    payload[1] = (int32_t)event->yuv_buffer_address;
    payload[2] = (int32_t)event->edmac_write_address;
    payload[3] = (int32_t)event->display_buffer_address;
    payload[4] = (int32_t)event->mvr_frame_number;
    payload[5] = (int32_t)event->packed_exposure;
    return 1;
}

int smoothaf_observability_serialize(
    const struct smoothaf_observability_event *event,
    uint8_t output[SMOOTHAF_OBS_RECORD_SIZE])
{
    int32_t payload[SMOOTHAF_OBS_PAYLOAD_SLOTS];
    if (!event || !output ||
        !smoothaf_observability_event_payload(event, payload)) return 0;
    obs_put_u32(output + 0, event->timestamp_ms);
    obs_put_u16(output + 4, event->event_type);
    obs_put_u16(output + 6, event->episode_id);
    obs_put_u32(output + 8, event->sequence);
    obs_put_u32(output + 12, (uint32_t)payload[0]);
    obs_put_u32(output + 16, (uint32_t)payload[1]);
    obs_put_u32(output + 20, (uint32_t)payload[2]);
    obs_put_u32(output + 24, (uint32_t)payload[3]);
    obs_put_u32(output + 28, (uint32_t)payload[4]);
    obs_put_u32(output + 32, (uint32_t)payload[5]);
    return 1;
}
