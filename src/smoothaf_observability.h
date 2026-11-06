#ifndef SMOOTHAF_OBSERVABILITY_H
#define SMOOTHAF_OBSERVABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compile-time default-off integration switch.  Define to 1 in a host/ARM
 * experiment build only; the production default contains no observer path. */
#ifndef SMOOTHAF_OBSERVABILITY_ENABLED
#define SMOOTHAF_OBSERVABILITY_ENABLED 0
#endif

#define SMOOTHAF_OBS_EVENT_TYPE 24u
#define SMOOTHAF_OBS_SCHEMA_VERSION 1u
#define SMOOTHAF_OBS_RECORD_SIZE 36u
#define SMOOTHAF_OBS_PAYLOAD_SLOTS 6u
#define SMOOTHAF_OBS_UINT32_SENTINEL UINT32_MAX
#define SMOOTHAF_OBS_UINT16_MAX UINT16_MAX
#define SMOOTHAF_OBS_UINT32_HALF 0x80000000u

#define SMOOTHAF_OBS_VERSION_SHIFT 0u
#define SMOOTHAF_OBS_PHASE_SHIFT 4u
#define SMOOTHAF_OBS_STATUS_SHIFT 5u
#define SMOOTHAF_OBS_PROFILE_SHIFT 7u
#define SMOOTHAF_OBS_FLAGS_SHIFT 8u
#define SMOOTHAF_OBS_LOGICAL_ID_SHIFT 16u

#define SMOOTHAF_OBS_VERSION_MASK 0x0Fu
#define SMOOTHAF_OBS_PHASE_MASK 0x01u
#define SMOOTHAF_OBS_STATUS_MASK 0x03u
#define SMOOTHAF_OBS_PROFILE_MASK 0x01u
#define SMOOTHAF_OBS_FLAGS_MASK 0xFFu
#define SMOOTHAF_OBS_LOGICAL_ID_MASK 0xFFFFu

/* B2-O3.1 field masks. */
#define SMOOTHAF_OBS_FIELD_YUV_BUFFER   (1u << 0)
#define SMOOTHAF_OBS_FIELD_GEOMETRY     (1u << 1)
#define SMOOTHAF_OBS_FIELD_LIVEVIEW     (1u << 2)
#define SMOOTHAF_OBS_FIELD_MOVIE_MODE   (1u << 3)
#define SMOOTHAF_OBS_FIELD_RECORDING    (1u << 4)
#define SMOOTHAF_OBS_FIELD_EDMAC        (1u << 5)
#define SMOOTHAF_OBS_FIELD_DISPLAY      (1u << 6)
#define SMOOTHAF_OBS_FIELD_EXPOSURE     (1u << 7)
#define SMOOTHAF_OBS_FIELD_MVR          (1u << 8)
#define SMOOTHAF_OBS_FIELD_TIMESTAMP    (1u << 9)

#define SMOOTHAF_OBS_REASON_ENVIRONMENT   (1u << 0)
#define SMOOTHAF_OBS_REASON_CANCELLED     (1u << 1)
#define SMOOTHAF_OBS_REASON_PANIC         (1u << 2)
#define SMOOTHAF_OBS_REASON_REQUIRED      (1u << 3)
#define SMOOTHAF_OBS_REASON_GEOMETRY      (1u << 4)
#define SMOOTHAF_OBS_REASON_TIMESTAMP     (1u << 5)
#define SMOOTHAF_OBS_REASON_MVR_COUNTER   (1u << 6)
#define SMOOTHAF_OBS_REASON_RECORDING     (1u << 7)

#define SMOOTHAF_OBS_FLAG_YUV_AVAILABLE       (1u << 0)
#define SMOOTHAF_OBS_FLAG_GEOMETRY_AVAILABLE  (1u << 1)
#define SMOOTHAF_OBS_FLAG_EDMAC_AVAILABLE    (1u << 2)
#define SMOOTHAF_OBS_FLAG_DISPLAY_AVAILABLE  (1u << 3)
#define SMOOTHAF_OBS_FLAG_EXPOSURE_AVAILABLE (1u << 4)
#define SMOOTHAF_OBS_FLAG_MVR_AVAILABLE      (1u << 5)
#define SMOOTHAF_OBS_FLAG_TIMESTAMP_WRAPPED  (1u << 6)
#define SMOOTHAF_OBS_FLAG_MVR_WRAPPED        (1u << 7)

#define SMOOTHAF_OBS_CORE_FIELDS \
    (SMOOTHAF_OBS_FIELD_YUV_BUFFER | SMOOTHAF_OBS_FIELD_GEOMETRY | \
     SMOOTHAF_OBS_FIELD_LIVEVIEW | SMOOTHAF_OBS_FIELD_MOVIE_MODE | \
     SMOOTHAF_OBS_FIELD_RECORDING | SMOOTHAF_OBS_FIELD_TIMESTAMP)
#define SMOOTHAF_OBS_EXTENDED_FIELDS \
    (SMOOTHAF_OBS_CORE_FIELDS | SMOOTHAF_OBS_FIELD_EDMAC | \
     SMOOTHAF_OBS_FIELD_DISPLAY | SMOOTHAF_OBS_FIELD_EXPOSURE | \
     SMOOTHAF_OBS_FIELD_MVR)

#define SMOOTHAF_OBS_EXTENDED_FLAGS \
    (SMOOTHAF_OBS_FLAG_EDMAC_AVAILABLE | SMOOTHAF_OBS_FLAG_DISPLAY_AVAILABLE | \
     SMOOTHAF_OBS_FLAG_EXPOSURE_AVAILABLE | SMOOTHAF_OBS_FLAG_MVR_AVAILABLE | \
     SMOOTHAF_OBS_FLAG_MVR_WRAPPED)

#define SMOOTHAF_OBS_STATIC_ASSERT(condition, name) \
    typedef char name[(condition) ? 1 : -1]
SMOOTHAF_OBS_STATIC_ASSERT(sizeof(uint8_t) == 1, obs_uint8_width);
SMOOTHAF_OBS_STATIC_ASSERT(sizeof(uint16_t) == 2, obs_uint16_width);
SMOOTHAF_OBS_STATIC_ASSERT(sizeof(uint32_t) == 4, obs_uint32_width);
SMOOTHAF_OBS_STATIC_ASSERT(SMOOTHAF_OBS_RECORD_SIZE ==
                           4u + 2u + 2u + 4u + 6u * 4u,
                           obs_record_wire_size);

enum smoothaf_observability_status {
    SMOOTHAF_OBS_STABLE_METADATA = 0,
    SMOOTHAF_OBS_CHANGED_DURING_MEASUREMENT = 1,
    SMOOTHAF_OBS_INSUFFICIENT_OBSERVABILITY = 2,
    SMOOTHAF_OBS_INVALID_ENVIRONMENT = 3,
};

enum smoothaf_observability_profile {
    SMOOTHAF_OBS_PROFILE_CORE = 0,
    SMOOTHAF_OBS_PROFILE_EXTENDED = 1,
};

enum smoothaf_observability_phase {
    SMOOTHAF_OBS_PHASE_BEFORE = 0,
    SMOOTHAF_OBS_PHASE_AFTER = 1,
};

enum smoothaf_observability_publish_action {
    SMOOTHAF_OBS_SUPPRESS = 0,
    SMOOTHAF_OBS_PUBLISH = 1,
};

/* Wire V1 has no namespace/generation bits.  These values are an in-process
 * pairing guard only; they are deliberately not added to the 36-byte ABI. */
enum smoothaf_observability_identity_namespace {
    SMOOTHAF_OBS_IDENTITY_NONE = 0,
    SMOOTHAF_OBS_IDENTITY_AF = 1,
    SMOOTHAF_OBS_IDENTITY_D1 = 2,
    SMOOTHAF_OBS_IDENTITY_MONITOR = 3,
    SMOOTHAF_OBS_IDENTITY_MANUAL = 4,
};

struct smoothaf_observability_exposure {
    uint8_t aperture;
    uint8_t shutter;
    uint8_t iso;
    uint8_t iso_auto;
};
SMOOTHAF_OBS_STATIC_ASSERT(sizeof(struct smoothaf_observability_exposure) == 4,
                           obs_exposure_layout);
SMOOTHAF_OBS_STATIC_ASSERT(offsetof(struct smoothaf_observability_exposure,
                                    aperture) == 0,
                           obs_exposure_aperture_offset);
SMOOTHAF_OBS_STATIC_ASSERT(offsetof(struct smoothaf_observability_exposure,
                                    iso_auto) == 3,
                           obs_exposure_iso_auto_offset);

struct smoothaf_observability_snapshot {
    uint32_t timestamp_ms;
    uint32_t available_mask;
    uint32_t invalid_reasons;
    uint32_t yuv_buffer_address;
    int32_t width;
    int32_t pitch;
    int32_t height;
    uint8_t liveview;
    uint8_t movie_mode;
    uint8_t recording_class;
    uint8_t environment_valid;
    uint32_t edmac_write_address;
    uint32_t display_buffer_address;
    struct smoothaf_observability_exposure exposure;
    uint32_t mvr_frame_number;
};

struct smoothaf_observability_result {
    enum smoothaf_observability_status status;
    enum smoothaf_observability_profile profile;
    uint32_t available_mask;
    uint32_t changed_mask;
    uint32_t missing_mask;
    uint32_t missing_required_mask;
    uint32_t invalid_mask;
    uint32_t invalid_reason_mask;
    uint32_t timestamp_delta_ms;
    uint8_t timestamp_wrapped;
    uint8_t timestamp_ambiguous;
    uint8_t mvr_wrapped;
    uint8_t mvr_non_monotonic;
};

struct smoothaf_observability_event {
    uint32_t timestamp_ms;
    uint16_t event_type;
    uint16_t episode_id;
    uint32_t sequence;
    /* Pairing identity is not serialized in V1.  The namespace/generation
     * prevents local logical-id reuse from crossing lifecycle boundaries. */
    uint8_t identity_namespace;
    uint32_t identity_generation;
    uint16_t logical_id;
    enum smoothaf_observability_phase phase;
    enum smoothaf_observability_status status;
    enum smoothaf_observability_profile profile;
    uint8_t flags;
    uint32_t yuv_buffer_address;
    uint32_t edmac_write_address;
    uint32_t display_buffer_address;
    uint32_t mvr_frame_number;
    uint32_t packed_exposure;
};

struct smoothaf_observability_controller {
    uint16_t next_logical_id;
    uint32_t minimum_window_ms;
    uint8_t identity_namespace;
    uint32_t identity_generation;
    uint8_t has_published;
    uint32_t last_timestamp_ms;
    enum smoothaf_observability_status last_status;
    uint8_t has_pending;
    struct smoothaf_observability_event pending_before;
    struct smoothaf_observability_event pending_after;
    uint32_t published_events;
    uint32_t dropped_events;
    uint32_t suppressed_observations;
    uint32_t incomplete_pairs;
};

typedef bool (*smoothaf_observability_publish_fn)(
    void *context, const struct smoothaf_observability_event *event);

void smoothaf_observability_snapshot_init(
    struct smoothaf_observability_snapshot *snapshot);

uint32_t smoothaf_observability_pack_exposure(
    const struct smoothaf_observability_exposure *exposure);

int smoothaf_observability_classify(
    const struct smoothaf_observability_snapshot *before,
    const struct smoothaf_observability_snapshot *after,
    enum smoothaf_observability_profile profile,
    struct smoothaf_observability_result *result);

void smoothaf_observability_controller_init(
    struct smoothaf_observability_controller *controller,
    uint32_t minimum_window_ms);

void smoothaf_observability_controller_set_identity(
    struct smoothaf_observability_controller *controller,
    uint8_t identity_namespace,
    uint32_t identity_generation);

void smoothaf_observability_event_set_identity(
    struct smoothaf_observability_event *event,
    uint8_t identity_namespace,
    uint32_t identity_generation);

uint16_t smoothaf_observability_next_logical_id(
    struct smoothaf_observability_controller *controller);

int smoothaf_observability_make_event(
    struct smoothaf_observability_event *event,
    const struct smoothaf_observability_snapshot *snapshot,
    const struct smoothaf_observability_result *result,
    uint16_t episode_id,
    uint32_t sequence,
    uint16_t logical_id,
    enum smoothaf_observability_phase phase);

int smoothaf_observability_submit_pair(
    struct smoothaf_observability_controller *controller,
    const struct smoothaf_observability_event *before,
    const struct smoothaf_observability_event *after,
    smoothaf_observability_publish_fn publish,
    void *context);

int smoothaf_observability_flush_pending(
    struct smoothaf_observability_controller *controller,
    smoothaf_observability_publish_fn publish,
    void *context);

int smoothaf_observability_event_payload(
    const struct smoothaf_observability_event *event,
    int32_t payload[SMOOTHAF_OBS_PAYLOAD_SLOTS]);

int smoothaf_observability_serialize(
    const struct smoothaf_observability_event *event,
    uint8_t output[SMOOTHAF_OBS_RECORD_SIZE]);

#endif
