#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_control_envelope.h"
#include "device_protocol_limits.h"
#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES 256U
#define DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES 128U
#define DEVICE_MEDIA_REASON_MAX_BYTES 128U

/* Text is borrowed from the envelope and must be copied before releasing it. */
typedef struct {
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    const char* text;
} device_media_transcript_t;

typedef struct {
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    char run_id[DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES + 1U];
    const char* text;
} device_media_text_output_t;

typedef enum {
    DEVICE_MEDIA_RUN_STATUS_ACCEPTED = 0,
    DEVICE_MEDIA_RUN_STATUS_RUNNING,
    DEVICE_MEDIA_RUN_STATUS_CANCELLING,
    DEVICE_MEDIA_RUN_STATUS_COMPLETED,
    DEVICE_MEDIA_RUN_STATUS_FAILED,
    DEVICE_MEDIA_RUN_STATUS_CANCELLED,
    DEVICE_MEDIA_RUN_STATUS_INTERRUPTED,
    DEVICE_MEDIA_RUN_STATUS_COMPACTION_REQUIRED,
} device_media_run_status_t;

typedef enum {
    DEVICE_MEDIA_INTERACTION_STATE_IDLE = 0,
    DEVICE_MEDIA_INTERACTION_STATE_LISTENING,
    DEVICE_MEDIA_INTERACTION_STATE_RECOGNIZING,
    DEVICE_MEDIA_INTERACTION_STATE_PROCESSING,
    DEVICE_MEDIA_INTERACTION_STATE_SPEAKING,
    DEVICE_MEDIA_INTERACTION_STATE_NEEDS_ATTENTION,
    DEVICE_MEDIA_INTERACTION_STATE_ERROR,
    DEVICE_MEDIA_INTERACTION_STATE_UNAVAILABLE,
} device_media_interaction_state_t;

typedef struct {
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    char input_id[DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES + 1U];
    char run_id[DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES + 1U];
    device_media_run_status_t queue_state;
} device_media_input_accepted_t;

typedef struct {
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    uint32_t stream_id;
} device_media_input_segment_accepted_t;

typedef struct {
    bool has_run_id;
    char run_id[DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES + 1U];
    bool has_client_input_id;
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    device_media_interaction_state_t state;
    bool has_reason;
    char reason[DEVICE_MEDIA_REASON_MAX_BYTES + 1U];
} device_media_state_changed_t;

typedef struct {
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    char run_id[DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES + 1U];
    uint32_t stream_id;
    uint64_t sample_count;
} device_media_playback_start_t;

typedef struct {
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    uint32_t stream_id;
    char reason[DEVICE_MEDIA_REASON_MAX_BYTES + 1U];
} device_media_playback_end_t;

device_protocol_result_t device_media_control_encode_listen_start(
    const char *message_id, const char *client_input_id, uint32_t stream_id,
    device_output_preference_t output_preference, char *encoded, size_t encoded_capacity,
    size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_listen_stop(const char *message_id, uint32_t stream_id,
                                        uint32_t last_sequence, char *encoded,
                                        size_t encoded_capacity, size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_listen_cancel(const char *message_id, uint32_t stream_id, char *encoded,
                                          size_t encoded_capacity, size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_playback_cancel(const char *message_id, const char *output_id,
                                            uint32_t stream_id, char *encoded,
                                            size_t encoded_capacity, size_t *encoded_size);

device_protocol_result_t
device_media_control_decode_input_accepted(const device_control_envelope_t *envelope,
                                           device_media_input_accepted_t *accepted);

device_protocol_result_t device_media_control_decode_input_segment_accepted(
    const device_control_envelope_t *envelope,
    device_media_input_segment_accepted_t *accepted);

device_protocol_result_t
device_media_control_decode_state_changed(const device_control_envelope_t *envelope,
                                          device_media_state_changed_t *state_changed);

device_protocol_result_t
device_media_control_decode_playback_start(const device_control_envelope_t *envelope,
                                           device_media_playback_start_t *playback_start);

device_protocol_result_t
device_media_control_decode_playback_end(const device_control_envelope_t *envelope,
                                         device_media_playback_end_t *playback_end);

device_protocol_result_t device_media_control_decode_transcript(
    const device_control_envelope_t* envelope, device_media_transcript_t* transcript);
device_protocol_result_t device_media_control_decode_text_output(
    const device_control_envelope_t* envelope, device_media_text_output_t* output);

#ifdef __cplusplus
}
#endif
