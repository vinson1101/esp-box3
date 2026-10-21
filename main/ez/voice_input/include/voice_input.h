#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_connection.h"
#include "device_media_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_INPUT_SAMPLE_RATE_HZ 16000U
#define VOICE_INPUT_FRAME_DURATION_MS 20U
#define VOICE_INPUT_SAMPLES_PER_FRAME \
    (VOICE_INPUT_SAMPLE_RATE_HZ * VOICE_INPUT_FRAME_DURATION_MS / 1000U)

typedef struct voice_input voice_input_t;

typedef enum {
    VOICE_INPUT_RESULT_OK = 0,
    VOICE_INPUT_RESULT_INVALID_ARGUMENT,
    VOICE_INPUT_RESULT_NO_MEMORY,
    VOICE_INPUT_RESULT_INVALID_STATE,
    VOICE_INPUT_RESULT_SESSION_MISMATCH,
    VOICE_INPUT_RESULT_CAPACITY_EXCEEDED,
} voice_input_result_t;

typedef enum {
    VOICE_INPUT_STATE_STOPPED = 0,
    VOICE_INPUT_STATE_IDLE,
    VOICE_INPUT_STATE_STARTING,
    VOICE_INPUT_STATE_RECORDING,
    VOICE_INPUT_STATE_SEALING,
    VOICE_INPUT_STATE_CANCELLING,
    VOICE_INPUT_STATE_SEALED,
    VOICE_INPUT_STATE_TRANSMITTING,
    VOICE_INPUT_STATE_AWAITING_ACCEPTANCE,
    VOICE_INPUT_STATE_FAILED,
} voice_input_state_t;

typedef enum {
    VOICE_INPUT_EVENT_RECORDING_STARTED = 0,
    VOICE_INPUT_EVENT_SEALED,
    VOICE_INPUT_EVENT_TRANSMISSION_STARTED,
    VOICE_INPUT_EVENT_AWAITING_ACCEPTANCE,
    VOICE_INPUT_EVENT_SEGMENT_ACCEPTED,
    VOICE_INPUT_EVENT_CANCELLED,
    VOICE_INPUT_EVENT_EXPIRED,
    VOICE_INPUT_EVENT_TOO_LONG,
    VOICE_INPUT_EVENT_FAILED,
    VOICE_INPUT_EVENT_REJECTED,
} voice_input_event_type_t;

typedef struct {
    voice_input_event_type_t type;
    voice_input_result_t result;
    uint64_t capture_session_id;
    uint64_t connection_epoch;
    uint64_t sample_count;
    uint64_t frame_count;
    uint32_t retransmission_count;
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
} voice_input_event_t;

/* Events run synchronously on the Voice Input task and must not block. */
typedef void (*voice_input_event_callback_t)(
    const voice_input_event_t *event,
    void *context);

typedef struct {
    device_connection_t *connection;
    device_output_preference_t output_preference;
    voice_input_event_callback_t event_callback;
    void *event_context;
} voice_input_config_t;

typedef struct {
    voice_input_state_t state;
    bool connection_online;
    uint64_t connection_epoch;
    uint64_t capture_session_id;
    uint64_t sample_count;
    uint64_t frame_count;
    size_t queued_frame_count;
    uint32_t retransmission_count;
    uint32_t accepted_segment_count;
    uint32_t rejected_input_count;
    uint32_t cancelled_input_count;
    uint32_t expired_input_count;
    uint32_t failed_input_count;
    uint32_t dropped_capture_frame_count;
} voice_input_status_t;

voice_input_result_t voice_input_create(
    const voice_input_config_t *config,
    voice_input_t **voice_input);

voice_input_result_t voice_input_start(voice_input_t *voice_input);

/* Reserves a new input session without blocking on protocol or audio I/O. */
voice_input_result_t voice_input_begin(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    uint64_t *capture_session_id);

/* Called by the Audio task; performs one bounded, non-blocking frame copy. */
bool voice_input_capture_frame(
    voice_input_t *voice_input,
    uint64_t capture_session_id,
    const int16_t samples[VOICE_INPUT_SAMPLES_PER_FRAME],
    size_t sample_count);

/* expected_frame_count orders sealing after every frame already produced by Audio. */
voice_input_result_t voice_input_seal(
    voice_input_t *voice_input,
    uint64_t capture_session_id,
    uint64_t expected_frame_count);

voice_input_result_t voice_input_cancel(
    voice_input_t *voice_input,
    uint64_t capture_session_id);

voice_input_result_t voice_input_connection_changed(
    voice_input_t *voice_input,
    bool online,
    uint64_t connection_epoch);

voice_input_result_t voice_input_segment_accept(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const device_media_input_segment_accepted_t *accepted);

/* Final Runtime acceptance also releases a matching cache if the segment ack was lost. */
voice_input_result_t voice_input_final_accept(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const device_media_input_accepted_t *accepted);

/* Releases a sealed input when Host returns it to idle without accepting it. */
voice_input_result_t voice_input_reject(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const char *client_input_id);

void voice_input_get_status(
    voice_input_t *voice_input,
    voice_input_status_t *status);

void voice_input_stop(voice_input_t *voice_input);

void voice_input_destroy(voice_input_t *voice_input);

#ifdef __cplusplus
}
#endif
