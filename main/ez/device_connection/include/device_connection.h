#ifndef DEVICE_CONNECTION_H
#define DEVICE_CONNECTION_H

#include <stdbool.h>
#include <stdint.h>

#include "device_authentication.h"
#include "device_capabilities.h"
#include "device_connection_supervisor.h"
#include "device_identity.h"
#include "device_media_control.h"
#include "device_pairing.h"
#include "device_pcm_frame.h"
#include "device_protocol_limits.h"
#include "host_discovery.h"
#include "wss_transport.h"

typedef struct device_connection device_connection_t;

typedef enum {
    DEVICE_CONNECTION_RESULT_OK = 0,
    DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT,
    DEVICE_CONNECTION_RESULT_NO_MEMORY,
    DEVICE_CONNECTION_RESULT_INVALID_STATE,
    DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED,
} device_connection_result_t;

typedef struct {
    bool paired;
    char installation_id[HOST_DISCOVERY_INSTALLATION_ID_LENGTH + 1U];
    char certificate_fingerprint[HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH + 1U];
} device_connection_identity_t;

typedef enum {
    DEVICE_CONNECTION_EVENT_STATE_CHANGED = 0,
    DEVICE_CONNECTION_EVENT_HOST_SELECTED,
    DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED,
    DEVICE_CONNECTION_EVENT_FAILURE,
    DEVICE_CONNECTION_EVENT_PAIRING_STATE_CHANGED,
} device_connection_event_type_t;

typedef struct {
    device_connection_event_type_t type;
    device_connection_state_t state;
    device_connection_block_t block_reason;
    device_connection_failure_t failure;
    host_selection_result_t selection_result;
    wss_transport_result_t transport_result;
    uint64_t network_ip_epoch;
    uint64_t connection_epoch;
    uint32_t reconnect_delay_ms;
    bool paired;
    char installation_id_suffix[9U];
    char message_id[DEVICE_MESSAGE_ID_MAX_BYTES + 1U];
    char message_type[DEVICE_MESSAGE_TYPE_MAX_BYTES + 1U];
    device_pairing_state_t pairing_state;
} device_connection_event_t;

/* Events run synchronously on the Network task and the callback must not block. */
typedef void (*device_connection_event_callback_t)(
    const device_connection_event_t *event,
    void *context);

typedef enum {
    DEVICE_CONNECTION_MEDIA_EVENT_INPUT_SEGMENT_ACCEPTED = 0,
    DEVICE_CONNECTION_MEDIA_EVENT_INPUT_ACCEPTED,
    DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED,
    DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_START,
    DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END,
    DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME,
    DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR,
    DEVICE_CONNECTION_MEDIA_EVENT_TRANSCRIPT,
    DEVICE_CONNECTION_MEDIA_EVENT_TEXT_OUTPUT,
} device_connection_media_event_type_t;

typedef struct {
    device_connection_media_event_type_t type;
    uint64_t connection_epoch;
    union {
        device_media_input_segment_accepted_t input_segment_accepted;
        device_media_input_accepted_t input_accepted;
        device_media_state_changed_t state_changed;
        device_media_playback_start_t playback_start;
        device_media_playback_end_t playback_end;
        device_pcm_frame_view_t pcm_frame;
        device_authentication_error_t remote_error;
        device_media_transcript_t transcript;
        device_media_text_output_t text_output;
    } value;
} device_connection_media_event_t;

/*
 * Runs synchronously on the Network task and must not block. PCM payload
 * storage remains valid only for the duration of the callback.
 */
typedef bool (*device_connection_media_callback_t)(
    const device_connection_media_event_t *event,
    void *context);

typedef struct {
    device_connection_identity_t identity;
    device_identity_t *identity_store;
    device_capabilities_t capabilities;
    device_output_preference_t output_preference;
    const char *client_version;
    const char *display_name;
    device_connection_event_callback_t event_callback;
    void *event_context;
    device_connection_media_callback_t media_callback;
    void *media_context;
} device_connection_config_t;

typedef struct {
    device_connection_state_t state;
    device_connection_block_t block_reason;
    bool network_has_ip;
    uint64_t network_ip_epoch;
    uint64_t connection_epoch;
    uint32_t consecutive_failures;
    uint32_t discovery_attempt_count;
    uint32_t transport_attempt_count;
    uint32_t control_message_count;
    uint32_t pcm_frame_count;
    uint32_t outbound_control_message_count;
    uint32_t outbound_pcm_frame_count;
    device_pairing_state_t pairing_state;
} device_connection_status_t;

device_connection_result_t device_connection_create(
    const device_connection_config_t *config,
    device_connection_t **connection);

device_connection_result_t device_connection_start(device_connection_t *connection);

device_connection_result_t device_connection_network_changed(
    device_connection_t *connection,
    bool has_ip,
    uint64_t ip_epoch);

device_connection_result_t device_connection_update_identity(
    device_connection_t *connection,
    const device_connection_identity_t *identity);

device_connection_result_t device_connection_copy_pairing_code(
    device_connection_t *connection,
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY]);

/* Outbound media is copied into the Network task queue without blocking. */
device_connection_result_t device_connection_send_listen_start(
    device_connection_t *connection,
    uint64_t connection_epoch,
    const char *client_input_id,
    uint32_t stream_id);

device_connection_result_t device_connection_send_pcm(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id,
    uint32_t sequence,
    const uint8_t pcm_payload[DEVICE_PCM_PAYLOAD_BYTES]);

device_connection_result_t device_connection_send_listen_stop(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id,
    uint32_t last_sequence);

device_connection_result_t device_connection_send_listen_cancel(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id);

device_connection_result_t device_connection_send_playback_cancel(
    device_connection_t *connection,
    uint64_t connection_epoch,
    const char *output_id,
    uint32_t stream_id);

void device_connection_get_status(
    device_connection_t *connection,
    device_connection_status_t *status);

void device_connection_stop(device_connection_t *connection);

void device_connection_destroy(device_connection_t *connection);

#endif
