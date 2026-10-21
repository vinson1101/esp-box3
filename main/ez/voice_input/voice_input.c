#include "voice_input.h"

#include <stdlib.h>
#include <string.h>

#include "device_pcm_frame.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "utterance_cache.h"

#define VOICE_INPUT_TASK_STACK_BYTES 8192U
#define VOICE_INPUT_TASK_PRIORITY 5U
#define VOICE_INPUT_FRAME_QUEUE_LENGTH 64U
#define VOICE_INPUT_CONTROL_QUEUE_LENGTH 16U
#define VOICE_INPUT_FRAME_BURST 16U
#define VOICE_INPUT_TRANSMISSION_BURST 4U
#define VOICE_INPUT_ACTIVE_POLL_MS 5U
#define VOICE_INPUT_IDLE_POLL_MS 250U
#define VOICE_INPUT_STOPPED_BIT BIT0
#define VOICE_INPUT_CLIENT_ID_RANDOM_BYTES 16U
#define VOICE_INPUT_CLIENT_ID_PREFIX "input-"
#define VOICE_INPUT_CLIENT_ID_BYTES \
    ((sizeof(VOICE_INPUT_CLIENT_ID_PREFIX) - 1U) + VOICE_INPUT_CLIENT_ID_RANDOM_BYTES * 2U)

_Static_assert(
    VOICE_INPUT_SAMPLES_PER_FRAME * sizeof(int16_t) == DEVICE_PCM_PAYLOAD_BYTES,
    "voice input must use the protocol PCM payload size");
_Static_assert(
    VOICE_INPUT_CLIENT_ID_BYTES <= DEVICE_CLIENT_INPUT_ID_MAX_BYTES,
    "voice input client ID must fit the device protocol");

typedef enum {
    VOICE_INPUT_CONTROL_BEGIN = 0,
    VOICE_INPUT_CONTROL_SEAL,
    VOICE_INPUT_CONTROL_CANCEL,
    VOICE_INPUT_CONTROL_CONNECTION_CHANGED,
    VOICE_INPUT_CONTROL_SEGMENT_ACCEPTED,
    VOICE_INPUT_CONTROL_FINAL_ACCEPTED,
    VOICE_INPUT_CONTROL_REJECTED,
} voice_input_control_type_t;

typedef struct {
    uint64_t capture_session_id;
    int16_t samples[VOICE_INPUT_SAMPLES_PER_FRAME];
} voice_input_frame_t;

typedef struct {
    voice_input_control_type_t type;
    union {
        struct {
            uint64_t capture_session_id;
            uint64_t connection_epoch;
            uint32_t stream_id;
            char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
        } begin;
        struct {
            uint64_t capture_session_id;
            uint64_t expected_frame_count;
        } seal;
        struct {
            uint64_t capture_session_id;
        } cancel;
        struct {
            bool online;
            uint64_t connection_epoch;
        } connection;
        struct {
            uint64_t connection_epoch;
            device_media_input_segment_accepted_t accepted;
        } segment_accepted;
        struct {
            uint64_t connection_epoch;
            device_media_input_accepted_t accepted;
        } final_accepted;
        struct {
            uint64_t connection_epoch;
            char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
        } rejected;
    } value;
} voice_input_control_t;

typedef enum {
    VOICE_INPUT_TRANSMISSION_NONE = 0,
    VOICE_INPUT_TRANSMISSION_CANCEL_PREVIOUS,
    VOICE_INPUT_TRANSMISSION_START,
    VOICE_INPUT_TRANSMISSION_FRAMES,
    VOICE_INPUT_TRANSMISSION_STOP,
} voice_input_transmission_phase_t;

struct voice_input {
    device_connection_t *connection;
    device_output_preference_t output_preference;
    voice_input_event_callback_t event_callback;
    void *event_context;
    utterance_cache_t *cache;
    QueueHandle_t frame_queue;
    StaticQueue_t frame_queue_state;
    uint8_t *frame_queue_storage;
    QueueHandle_t control_queue;
    StaticQueue_t control_queue_state;
    uint8_t *control_queue_storage;
    EventGroupHandle_t lifecycle_events;
    TaskHandle_t task;
    portMUX_TYPE status_lock;
    bool started;
    bool stop_requested;
    voice_input_status_t status;

    bool connection_online;
    uint64_t connection_epoch;
    uint64_t capture_session_id;
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    uint64_t cached_sample_count;
    uint64_t cached_frame_count;
    bool seal_requested;
    uint64_t expected_frame_count;
    bool live_stream_active;
    uint64_t stream_connection_epoch;
    uint32_t stream_id;
    uint32_t live_next_sequence;
    voice_input_transmission_phase_t transmission_phase;
    uint32_t transmission_stream_id;
    uint32_t transmission_sequence;
    bool cancel_pending;
    uint64_t cancel_connection_epoch;
    uint32_t cancel_stream_id;
};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static bool output_preference_valid(device_output_preference_t output_preference)
{
    return output_preference == DEVICE_OUTPUT_PREFERENCE_TEXT ||
           output_preference == DEVICE_OUTPUT_PREFERENCE_AUDIO ||
           output_preference == DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
}

static uint32_t random_stream_id(void)
{
    uint32_t stream_id = 0U;
    while (stream_id == 0U) {
        stream_id = esp_random();
    }
    return stream_id;
}

static uint64_t random_capture_session_id(void)
{
    uint64_t capture_session_id = 0U;
    while (capture_session_id == 0U) {
        esp_fill_random(&capture_session_id, sizeof(capture_session_id));
    }
    return capture_session_id;
}

static void random_client_input_id(
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[VOICE_INPUT_CLIENT_ID_RANDOM_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    memcpy(
        client_input_id,
        VOICE_INPUT_CLIENT_ID_PREFIX,
        sizeof(VOICE_INPUT_CLIENT_ID_PREFIX) - 1U);
    size_t offset = sizeof(VOICE_INPUT_CLIENT_ID_PREFIX) - 1U;
    for (size_t index = 0U; index < sizeof(random_bytes); ++index) {
        client_input_id[offset++] = hex[random_bytes[index] >> 4U];
        client_input_id[offset++] = hex[random_bytes[index] & 0x0fU];
    }
    client_input_id[offset] = '\0';
    memset(random_bytes, 0, sizeof(random_bytes));
}

static void status_publish(voice_input_t *voice_input, voice_input_state_t state)
{
    taskENTER_CRITICAL(&voice_input->status_lock);
    voice_input->status.state = state;
    voice_input->status.connection_online = voice_input->connection_online;
    voice_input->status.connection_epoch = voice_input->connection_epoch;
    voice_input->status.capture_session_id = voice_input->capture_session_id;
    voice_input->status.sample_count = voice_input->cached_sample_count;
    voice_input->status.frame_count = voice_input->cached_frame_count;
    taskEXIT_CRITICAL(&voice_input->status_lock);
}

static void emit_event(
    voice_input_t *voice_input,
    voice_input_event_type_t type,
    voice_input_result_t result)
{
    voice_input_event_t event = {
        .type = type,
        .result = result,
        .capture_session_id = voice_input->capture_session_id,
        .connection_epoch = voice_input->connection_epoch,
        .sample_count = voice_input->cached_sample_count,
        .frame_count = voice_input->cached_frame_count,
        .retransmission_count = voice_input->status.retransmission_count,
    };
    memcpy(event.client_input_id, voice_input->client_input_id, sizeof(event.client_input_id));
    if (voice_input->event_callback != NULL) {
        voice_input->event_callback(&event, voice_input->event_context);
    }
}

static void input_clear(voice_input_t *voice_input, voice_input_state_t next_state)
{
    utterance_cache_clear(voice_input->cache);
    voice_input->capture_session_id = 0U;
    memset(voice_input->client_input_id, 0, sizeof(voice_input->client_input_id));
    voice_input->cached_sample_count = 0U;
    voice_input->cached_frame_count = 0U;
    voice_input->seal_requested = false;
    voice_input->expected_frame_count = 0U;
    voice_input->live_stream_active = false;
    voice_input->stream_connection_epoch = 0U;
    voice_input->stream_id = 0U;
    voice_input->live_next_sequence = 0U;
    voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_NONE;
    voice_input->transmission_stream_id = 0U;
    voice_input->transmission_sequence = 0U;
    status_publish(voice_input, next_state);
}

static void transmission_abandon(voice_input_t *voice_input)
{
    voice_input->live_stream_active = false;
    voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_NONE;
    voice_input->transmission_stream_id = 0U;
    voice_input->transmission_sequence = 0U;
    if (utterance_cache_state(voice_input->cache) == UTTERANCE_CACHE_STATE_SEALED) {
        status_publish(voice_input, VOICE_INPUT_STATE_SEALED);
    }
}

static void transmission_schedule(voice_input_t *voice_input)
{
    if (!voice_input->connection_online ||
        utterance_cache_state(voice_input->cache) != UTTERANCE_CACHE_STATE_SEALED) {
        return;
    }
    voice_input->transmission_phase =
        voice_input->stream_id != 0U &&
                voice_input->stream_connection_epoch == voice_input->connection_epoch
            ? VOICE_INPUT_TRANSMISSION_CANCEL_PREVIOUS
            : VOICE_INPUT_TRANSMISSION_START;
    voice_input->transmission_stream_id = random_stream_id();
    voice_input->transmission_sequence = 0U;
    taskENTER_CRITICAL(&voice_input->status_lock);
    ++voice_input->status.retransmission_count;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    status_publish(voice_input, VOICE_INPUT_STATE_TRANSMITTING);
    emit_event(
        voice_input,
        VOICE_INPUT_EVENT_TRANSMISSION_STARTED,
        VOICE_INPUT_RESULT_OK);
}

static bool transport_capacity_available(device_connection_result_t result)
{
    return result == DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED;
}

static void cancel_schedule(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    uint32_t stream_id)
{
    if (!voice_input->connection_online || connection_epoch == 0U || stream_id == 0U ||
        connection_epoch != voice_input->connection_epoch) {
        return;
    }
    const device_connection_result_t result = device_connection_send_listen_cancel(
        voice_input->connection,
        connection_epoch,
        stream_id);
    if (transport_capacity_available(result)) {
        voice_input->cancel_pending = true;
        voice_input->cancel_connection_epoch = connection_epoch;
        voice_input->cancel_stream_id = stream_id;
    }
}

static void cancel_step(voice_input_t *voice_input)
{
    if (!voice_input->cancel_pending) {
        return;
    }
    if (!voice_input->connection_online ||
        voice_input->connection_epoch != voice_input->cancel_connection_epoch) {
        voice_input->cancel_pending = false;
    } else {
        const device_connection_result_t result = device_connection_send_listen_cancel(
            voice_input->connection,
            voice_input->cancel_connection_epoch,
            voice_input->cancel_stream_id);
        if (transport_capacity_available(result)) {
            return;
        }
        voice_input->cancel_pending = false;
    }
    voice_input->cancel_connection_epoch = 0U;
    voice_input->cancel_stream_id = 0U;
    if (utterance_cache_state(voice_input->cache) == UTTERANCE_CACHE_STATE_EMPTY) {
        status_publish(voice_input, VOICE_INPUT_STATE_IDLE);
    }
}

static void transmission_step(voice_input_t *voice_input)
{
    if (voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_NONE ||
        !voice_input->connection_online) {
        return;
    }
    if (voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_CANCEL_PREVIOUS) {
        const device_connection_result_t result = device_connection_send_listen_cancel(
            voice_input->connection,
            voice_input->connection_epoch,
            voice_input->stream_id);
        if (transport_capacity_available(result)) {
            return;
        }
        if (result != DEVICE_CONNECTION_RESULT_OK) {
            transmission_abandon(voice_input);
            return;
        }
        voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_START;
    }
    if (voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_START) {
        const device_connection_result_t result = device_connection_send_listen_start(
            voice_input->connection,
            voice_input->connection_epoch,
            voice_input->client_input_id,
            voice_input->transmission_stream_id);
        if (transport_capacity_available(result)) {
            return;
        }
        if (result != DEVICE_CONNECTION_RESULT_OK) {
            transmission_abandon(voice_input);
            return;
        }
        voice_input->stream_connection_epoch = voice_input->connection_epoch;
        voice_input->stream_id = voice_input->transmission_stream_id;
        voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_FRAMES;
    }
    if (voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_FRAMES) {
        utterance_cache_view_t view = {0};
        if (utterance_cache_copy_view(voice_input->cache, &view) !=
            UTTERANCE_CACHE_RESULT_OK) {
            transmission_abandon(voice_input);
            return;
        }
        const uint32_t frame_count = (uint32_t)(view.pcm_size / DEVICE_PCM_PAYLOAD_BYTES);
        for (size_t burst = 0U;
             burst < VOICE_INPUT_TRANSMISSION_BURST &&
             voice_input->transmission_sequence < frame_count;
             ++burst) {
            const uint8_t *payload =
                &view.pcm[(size_t)voice_input->transmission_sequence * DEVICE_PCM_PAYLOAD_BYTES];
            const device_connection_result_t result = device_connection_send_pcm(
                voice_input->connection,
                voice_input->connection_epoch,
                voice_input->stream_id,
                voice_input->transmission_sequence,
                payload);
            if (transport_capacity_available(result)) {
                return;
            }
            if (result != DEVICE_CONNECTION_RESULT_OK) {
                transmission_abandon(voice_input);
                return;
            }
            ++voice_input->transmission_sequence;
        }
        if (voice_input->transmission_sequence == frame_count) {
            voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_STOP;
        }
    }
    if (voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_STOP) {
        if (voice_input->transmission_sequence == 0U) {
            transmission_abandon(voice_input);
            return;
        }
        const device_connection_result_t result = device_connection_send_listen_stop(
            voice_input->connection,
            voice_input->connection_epoch,
            voice_input->stream_id,
            voice_input->transmission_sequence - 1U);
        if (transport_capacity_available(result)) {
            return;
        }
        if (result != DEVICE_CONNECTION_RESULT_OK) {
            transmission_abandon(voice_input);
            return;
        }
        voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_NONE;
        status_publish(voice_input, VOICE_INPUT_STATE_AWAITING_ACCEPTANCE);
        emit_event(
            voice_input,
            VOICE_INPUT_EVENT_AWAITING_ACCEPTANCE,
            VOICE_INPUT_RESULT_OK);
    }
}

static void input_cancel_current(
    voice_input_t *voice_input,
    voice_input_event_type_t event_type,
    voice_input_result_t result)
{
    cancel_schedule(
        voice_input,
        voice_input->stream_connection_epoch,
        voice_input->stream_id);
    if (event_type == VOICE_INPUT_EVENT_CANCELLED) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        ++voice_input->status.cancelled_input_count;
        taskEXIT_CRITICAL(&voice_input->status_lock);
    } else if (event_type == VOICE_INPUT_EVENT_EXPIRED) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        ++voice_input->status.expired_input_count;
        taskEXIT_CRITICAL(&voice_input->status_lock);
    } else {
        taskENTER_CRITICAL(&voice_input->status_lock);
        ++voice_input->status.failed_input_count;
        taskEXIT_CRITICAL(&voice_input->status_lock);
    }
    emit_event(voice_input, event_type, result);
    input_clear(
        voice_input,
        voice_input->cancel_pending ? VOICE_INPUT_STATE_CANCELLING
                                    : VOICE_INPUT_STATE_IDLE);
}

static void sealing_complete(voice_input_t *voice_input)
{
    if (!voice_input->seal_requested ||
        voice_input->cached_frame_count < voice_input->expected_frame_count) {
        return;
    }
    if (voice_input->cached_frame_count != voice_input->expected_frame_count ||
        utterance_cache_seal(voice_input->cache, monotonic_ms()) !=
            UTTERANCE_CACHE_RESULT_OK) {
        input_cancel_current(
            voice_input,
            VOICE_INPUT_EVENT_FAILED,
            VOICE_INPUT_RESULT_INVALID_STATE);
        return;
    }
    voice_input->seal_requested = false;
    status_publish(voice_input, VOICE_INPUT_STATE_SEALED);
    emit_event(voice_input, VOICE_INPUT_EVENT_SEALED, VOICE_INPUT_RESULT_OK);

    if (voice_input->connection_online && voice_input->live_stream_active &&
        voice_input->stream_connection_epoch == voice_input->connection_epoch &&
        voice_input->live_next_sequence == voice_input->cached_frame_count) {
        const device_connection_result_t result = device_connection_send_listen_stop(
            voice_input->connection,
            voice_input->connection_epoch,
            voice_input->stream_id,
            voice_input->live_next_sequence - 1U);
        if (result == DEVICE_CONNECTION_RESULT_OK) {
            status_publish(voice_input, VOICE_INPUT_STATE_AWAITING_ACCEPTANCE);
            emit_event(
                voice_input,
                VOICE_INPUT_EVENT_AWAITING_ACCEPTANCE,
                VOICE_INPUT_RESULT_OK);
            return;
        }
    }
    voice_input->live_stream_active = false;
    transmission_schedule(voice_input);
}

static void frame_process(voice_input_t *voice_input, const voice_input_frame_t *frame)
{
    if (frame->capture_session_id != voice_input->capture_session_id ||
        utterance_cache_state(voice_input->cache) != UTTERANCE_CACHE_STATE_RECORDING) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        ++voice_input->status.dropped_capture_frame_count;
        taskEXIT_CRITICAL(&voice_input->status_lock);
        return;
    }
    const utterance_cache_result_t append_result = utterance_cache_append(
        voice_input->cache,
        frame->samples,
        VOICE_INPUT_SAMPLES_PER_FRAME);
    if (append_result != UTTERANCE_CACHE_RESULT_OK) {
        input_cancel_current(
            voice_input,
            append_result == UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED
                ? VOICE_INPUT_EVENT_TOO_LONG
                : VOICE_INPUT_EVENT_FAILED,
            append_result == UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED
                ? VOICE_INPUT_RESULT_CAPACITY_EXCEEDED
                : VOICE_INPUT_RESULT_INVALID_STATE);
        return;
    }
    voice_input->cached_sample_count += VOICE_INPUT_SAMPLES_PER_FRAME;
    ++voice_input->cached_frame_count;
    status_publish(
        voice_input,
        voice_input->seal_requested ? VOICE_INPUT_STATE_SEALING
                                    : VOICE_INPUT_STATE_RECORDING);

    if (voice_input->connection_online && voice_input->live_stream_active &&
        voice_input->stream_connection_epoch == voice_input->connection_epoch) {
        const device_connection_result_t result = device_connection_send_pcm(
            voice_input->connection,
            voice_input->connection_epoch,
            voice_input->stream_id,
            voice_input->live_next_sequence,
            (const uint8_t *)frame->samples);
        if (result == DEVICE_CONNECTION_RESULT_OK) {
            ++voice_input->live_next_sequence;
        } else {
            voice_input->live_stream_active = false;
        }
    }
    sealing_complete(voice_input);
}

static void begin_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    if (utterance_cache_state(voice_input->cache) != UTTERANCE_CACHE_STATE_EMPTY) {
        input_cancel_current(
            voice_input,
            VOICE_INPUT_EVENT_FAILED,
            VOICE_INPUT_RESULT_INVALID_STATE);
        return;
    }
    voice_input->capture_session_id = control->value.begin.capture_session_id;
    memcpy(
        voice_input->client_input_id,
        control->value.begin.client_input_id,
        sizeof(voice_input->client_input_id));
    voice_input->stream_connection_epoch = control->value.begin.connection_epoch;
    voice_input->stream_id = control->value.begin.stream_id;
    voice_input->live_next_sequence = 0U;
    const utterance_cache_result_t begin_result = utterance_cache_begin(
        voice_input->cache,
        voice_input->client_input_id,
        voice_input->output_preference);
    if (begin_result != UTTERANCE_CACHE_RESULT_OK) {
        input_cancel_current(
            voice_input,
            VOICE_INPUT_EVENT_FAILED,
            VOICE_INPUT_RESULT_INVALID_STATE);
        return;
    }
    voice_input->live_stream_active =
        voice_input->connection_online &&
        voice_input->connection_epoch == voice_input->stream_connection_epoch &&
        device_connection_send_listen_start(
            voice_input->connection,
            voice_input->stream_connection_epoch,
            voice_input->client_input_id,
            voice_input->stream_id) == DEVICE_CONNECTION_RESULT_OK;
    status_publish(voice_input, VOICE_INPUT_STATE_RECORDING);
    emit_event(
        voice_input,
        VOICE_INPUT_EVENT_RECORDING_STARTED,
        VOICE_INPUT_RESULT_OK);
}

static void connection_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    const bool online = control->value.connection.online;
    const uint64_t connection_epoch = control->value.connection.connection_epoch;
    const bool connection_changed = !voice_input->connection_online ||
                                    voice_input->connection_epoch != connection_epoch;
    if (online) {
        voice_input->connection_online = true;
        voice_input->connection_epoch = connection_epoch;
    } else if (voice_input->connection_epoch == connection_epoch) {
        voice_input->connection_online = false;
        voice_input->live_stream_active = false;
        voice_input->transmission_phase = VOICE_INPUT_TRANSMISSION_NONE;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    voice_input->status.connection_online = voice_input->connection_online;
    voice_input->status.connection_epoch = voice_input->connection_epoch;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    if (online && connection_changed &&
        utterance_cache_state(voice_input->cache) ==
                      UTTERANCE_CACHE_STATE_SEALED) {
        transmission_schedule(voice_input);
    } else if (!online && utterance_cache_state(voice_input->cache) ==
                           UTTERANCE_CACHE_STATE_SEALED) {
        status_publish(voice_input, VOICE_INPUT_STATE_SEALED);
    }
}

static void segment_accepted_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    if (control->value.segment_accepted.connection_epoch != voice_input->connection_epoch ||
        utterance_cache_state(voice_input->cache) == UTTERANCE_CACHE_STATE_EMPTY ||
        control->value.segment_accepted.accepted.stream_id != voice_input->stream_id ||
        strcmp(control->value.segment_accepted.accepted.client_input_id,
               voice_input->client_input_id) != 0) {
        return;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    ++voice_input->status.accepted_segment_count;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    emit_event(voice_input, VOICE_INPUT_EVENT_SEGMENT_ACCEPTED, VOICE_INPUT_RESULT_OK);
    input_clear(voice_input, VOICE_INPUT_STATE_IDLE);
}

static void final_accepted_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    if (control->value.final_accepted.connection_epoch != voice_input->connection_epoch ||
        utterance_cache_state(voice_input->cache) == UTTERANCE_CACHE_STATE_EMPTY ||
        strcmp(control->value.final_accepted.accepted.client_input_id,
               voice_input->client_input_id) != 0) {
        return;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    ++voice_input->status.accepted_segment_count;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    emit_event(voice_input, VOICE_INPUT_EVENT_SEGMENT_ACCEPTED, VOICE_INPUT_RESULT_OK);
    input_clear(voice_input, VOICE_INPUT_STATE_IDLE);
}

static void rejected_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    if (control->value.rejected.connection_epoch != voice_input->connection_epoch ||
        utterance_cache_state(voice_input->cache) != UTTERANCE_CACHE_STATE_SEALED ||
        strcmp(control->value.rejected.client_input_id,
               voice_input->client_input_id) != 0) {
        return;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    ++voice_input->status.rejected_input_count;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    emit_event(voice_input, VOICE_INPUT_EVENT_REJECTED, VOICE_INPUT_RESULT_OK);
    input_clear(voice_input, VOICE_INPUT_STATE_IDLE);
}

static void control_process(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    switch (control->type) {
        case VOICE_INPUT_CONTROL_BEGIN:
            begin_process(voice_input, control);
            break;
        case VOICE_INPUT_CONTROL_SEAL:
            if (control->value.seal.capture_session_id ==
                    voice_input->capture_session_id &&
                utterance_cache_state(voice_input->cache) ==
                    UTTERANCE_CACHE_STATE_RECORDING) {
                voice_input->seal_requested = true;
                voice_input->expected_frame_count =
                    control->value.seal.expected_frame_count;
                status_publish(voice_input, VOICE_INPUT_STATE_SEALING);
                sealing_complete(voice_input);
            }
            break;
        case VOICE_INPUT_CONTROL_CANCEL:
            if (control->value.cancel.capture_session_id ==
                voice_input->capture_session_id) {
                input_cancel_current(
                    voice_input,
                    VOICE_INPUT_EVENT_CANCELLED,
                    VOICE_INPUT_RESULT_OK);
            }
            break;
        case VOICE_INPUT_CONTROL_CONNECTION_CHANGED:
            connection_process(voice_input, control);
            break;
        case VOICE_INPUT_CONTROL_SEGMENT_ACCEPTED:
            segment_accepted_process(voice_input, control);
            break;
        case VOICE_INPUT_CONTROL_FINAL_ACCEPTED:
            final_accepted_process(voice_input, control);
            break;
        case VOICE_INPUT_CONTROL_REJECTED:
            rejected_process(voice_input, control);
            break;
        default:
            break;
    }
}

static void expiry_process(voice_input_t *voice_input)
{
    if (utterance_cache_is_expired(voice_input->cache, monotonic_ms())) {
        input_cancel_current(
            voice_input,
            VOICE_INPUT_EVENT_EXPIRED,
            VOICE_INPUT_RESULT_OK);
    }
}

static void voice_input_task(void *context)
{
    voice_input_t *voice_input = context;
    while (true) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        const bool stop_requested = voice_input->stop_requested;
        taskEXIT_CRITICAL(&voice_input->status_lock);
        if (stop_requested) {
            break;
        }

        voice_input_control_t control;
        while (xQueueReceive(voice_input->control_queue, &control, 0U) == pdTRUE) {
            control_process(voice_input, &control);
            memset(&control, 0, sizeof(control));
        }
        voice_input_frame_t frame;
        for (size_t index = 0U; index < VOICE_INPUT_FRAME_BURST; ++index) {
            if (xQueueReceive(voice_input->frame_queue, &frame, 0U) != pdTRUE) {
                break;
            }
            frame_process(voice_input, &frame);
            memset(&frame, 0, sizeof(frame));
        }
        cancel_step(voice_input);
        transmission_step(voice_input);
        expiry_process(voice_input);

        if (uxQueueMessagesWaiting(voice_input->control_queue) != 0U ||
            uxQueueMessagesWaiting(voice_input->frame_queue) != 0U) {
            continue;
        }

        const TickType_t wait_ticks = pdMS_TO_TICKS(
            voice_input->transmission_phase == VOICE_INPUT_TRANSMISSION_NONE
                ? VOICE_INPUT_IDLE_POLL_MS
                : VOICE_INPUT_ACTIVE_POLL_MS);
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks == 0U ? 1U : wait_ticks);
    }

    (void)xQueueReset(voice_input->frame_queue);
    (void)xQueueReset(voice_input->control_queue);
    input_clear(voice_input, VOICE_INPUT_STATE_STOPPED);
    taskENTER_CRITICAL(&voice_input->status_lock);
    voice_input->started = false;
    voice_input->stop_requested = false;
    voice_input->task = NULL;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    xEventGroupSetBits(voice_input->lifecycle_events, VOICE_INPUT_STOPPED_BIT);
    vTaskDelete(NULL);
}

voice_input_result_t voice_input_create(
    const voice_input_config_t *config,
    voice_input_t **voice_input)
{
    if (config == NULL || config->connection == NULL ||
        !output_preference_valid(config->output_preference) ||
        voice_input == NULL || *voice_input != NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    voice_input_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return VOICE_INPUT_RESULT_NO_MEMORY;
    }
    created->status_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    created->connection = config->connection;
    created->output_preference = config->output_preference;
    created->event_callback = config->event_callback;
    created->event_context = config->event_context;
    created->frame_queue_storage = heap_caps_malloc(
        VOICE_INPUT_FRAME_QUEUE_LENGTH * sizeof(voice_input_frame_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    created->control_queue_storage = heap_caps_malloc(
        VOICE_INPUT_CONTROL_QUEUE_LENGTH * sizeof(voice_input_control_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created->frame_queue_storage != NULL) {
        created->frame_queue = xQueueCreateStatic(
            VOICE_INPUT_FRAME_QUEUE_LENGTH,
            sizeof(voice_input_frame_t),
            created->frame_queue_storage,
            &created->frame_queue_state);
    }
    if (created->control_queue_storage != NULL) {
        created->control_queue = xQueueCreateStatic(
            VOICE_INPUT_CONTROL_QUEUE_LENGTH,
            sizeof(voice_input_control_t),
            created->control_queue_storage,
            &created->control_queue_state);
    }
    created->lifecycle_events = xEventGroupCreate();
    if (created->frame_queue == NULL || created->control_queue == NULL ||
        created->lifecycle_events == NULL ||
        utterance_cache_create(&created->cache) != UTTERANCE_CACHE_RESULT_OK) {
        if (created->lifecycle_events != NULL) {
            vEventGroupDelete(created->lifecycle_events);
        }
        if (created->frame_queue != NULL) {
            vQueueDelete(created->frame_queue);
        }
        if (created->control_queue != NULL) {
            vQueueDelete(created->control_queue);
        }
        free(created->frame_queue_storage);
        free(created->control_queue_storage);
        utterance_cache_destroy(created->cache);
        free(created);
        return VOICE_INPUT_RESULT_NO_MEMORY;
    }
    created->status.state = VOICE_INPUT_STATE_STOPPED;
    *voice_input = created;
    return VOICE_INPUT_RESULT_OK;
}

voice_input_result_t voice_input_start(voice_input_t *voice_input)
{
    if (voice_input == NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    if (voice_input->started) {
        taskEXIT_CRITICAL(&voice_input->status_lock);
        return VOICE_INPUT_RESULT_INVALID_STATE;
    }
    voice_input->started = true;
    voice_input->stop_requested = false;
    voice_input->status.state = VOICE_INPUT_STATE_IDLE;
    xEventGroupClearBits(voice_input->lifecycle_events, VOICE_INPUT_STOPPED_BIT);
    taskEXIT_CRITICAL(&voice_input->status_lock);
    const BaseType_t task_result = xTaskCreate(
        voice_input_task,
        "voice_input",
        VOICE_INPUT_TASK_STACK_BYTES,
        voice_input,
        VOICE_INPUT_TASK_PRIORITY,
        &voice_input->task);
    if (task_result != pdPASS) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        voice_input->started = false;
        voice_input->status.state = VOICE_INPUT_STATE_STOPPED;
        taskEXIT_CRITICAL(&voice_input->status_lock);
        return VOICE_INPUT_RESULT_NO_MEMORY;
    }
    return VOICE_INPUT_RESULT_OK;
}

voice_input_result_t voice_input_begin(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    uint64_t *capture_session_id)
{
    if (voice_input == NULL || connection_epoch == 0U ||
        capture_session_id == NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_BEGIN,
        .value.begin = {
            .capture_session_id = random_capture_session_id(),
            .connection_epoch = connection_epoch,
            .stream_id = random_stream_id(),
        },
    };
    random_client_input_id(control.value.begin.client_input_id);
    taskENTER_CRITICAL(&voice_input->status_lock);
    const bool available = voice_input->started &&
                           voice_input->status.state == VOICE_INPUT_STATE_IDLE &&
                           voice_input->status.connection_online &&
                           voice_input->status.connection_epoch == connection_epoch;
    if (!available) {
        taskEXIT_CRITICAL(&voice_input->status_lock);
        memset(&control, 0, sizeof(control));
        return VOICE_INPUT_RESULT_INVALID_STATE;
    }
    voice_input->status.state = VOICE_INPUT_STATE_STARTING;
    voice_input->status.capture_session_id =
        control.value.begin.capture_session_id;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    if (xQueueSend(voice_input->control_queue, &control, 0U) != pdTRUE) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        voice_input->status.state = VOICE_INPUT_STATE_IDLE;
        voice_input->status.capture_session_id = 0U;
        taskEXIT_CRITICAL(&voice_input->status_lock);
        memset(&control, 0, sizeof(control));
        return VOICE_INPUT_RESULT_CAPACITY_EXCEEDED;
    }
    *capture_session_id = control.value.begin.capture_session_id;
    memset(&control, 0, sizeof(control));
    xTaskNotifyGive(voice_input->task);
    return VOICE_INPUT_RESULT_OK;
}

bool voice_input_capture_frame(
    voice_input_t *voice_input,
    uint64_t capture_session_id,
    const int16_t samples[VOICE_INPUT_SAMPLES_PER_FRAME],
    size_t sample_count)
{
    if (voice_input == NULL || capture_session_id == 0U || samples == NULL ||
        sample_count != VOICE_INPUT_SAMPLES_PER_FRAME) {
        return false;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    const bool accepting = voice_input->started &&
                           (voice_input->status.state == VOICE_INPUT_STATE_STARTING ||
                            voice_input->status.state == VOICE_INPUT_STATE_RECORDING ||
                            voice_input->status.state == VOICE_INPUT_STATE_SEALING) &&
                           voice_input->status.capture_session_id == capture_session_id;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    if (!accepting) {
        return false;
    }
    voice_input_frame_t frame = {.capture_session_id = capture_session_id};
    memcpy(frame.samples, samples, sizeof(frame.samples));
    if (xQueueSend(voice_input->frame_queue, &frame, 0U) != pdTRUE) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        ++voice_input->status.dropped_capture_frame_count;
        taskEXIT_CRITICAL(&voice_input->status_lock);
        memset(&frame, 0, sizeof(frame));
        return false;
    }
    memset(&frame, 0, sizeof(frame));
    xTaskNotifyGive(voice_input->task);
    return true;
}

static voice_input_result_t control_enqueue(
    voice_input_t *voice_input,
    const voice_input_control_t *control)
{
    if (xQueueSend(voice_input->control_queue, control, 0U) != pdTRUE) {
        return VOICE_INPUT_RESULT_CAPACITY_EXCEEDED;
    }
    xTaskNotifyGive(voice_input->task);
    return VOICE_INPUT_RESULT_OK;
}

voice_input_result_t voice_input_seal(
    voice_input_t *voice_input,
    uint64_t capture_session_id,
    uint64_t expected_frame_count)
{
    if (voice_input == NULL || capture_session_id == 0U ||
        expected_frame_count == 0U) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    const bool matching = voice_input->started &&
                          (voice_input->status.state == VOICE_INPUT_STATE_STARTING ||
                           voice_input->status.state == VOICE_INPUT_STATE_RECORDING) &&
                          voice_input->status.capture_session_id == capture_session_id;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    if (!matching) {
        return VOICE_INPUT_RESULT_SESSION_MISMATCH;
    }
    const voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_SEAL,
        .value.seal = {
            .capture_session_id = capture_session_id,
            .expected_frame_count = expected_frame_count,
        },
    };
    return control_enqueue(voice_input, &control);
}

voice_input_result_t voice_input_cancel(
    voice_input_t *voice_input,
    uint64_t capture_session_id)
{
    if (voice_input == NULL || capture_session_id == 0U) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    const voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_CANCEL,
        .value.cancel.capture_session_id = capture_session_id,
    };
    return control_enqueue(voice_input, &control);
}

voice_input_result_t voice_input_connection_changed(
    voice_input_t *voice_input,
    bool online,
    uint64_t connection_epoch)
{
    if (voice_input == NULL || connection_epoch == 0U) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    if (!voice_input->started) {
        taskEXIT_CRITICAL(&voice_input->status_lock);
        return VOICE_INPUT_RESULT_INVALID_STATE;
    }
    const bool previous_online = voice_input->status.connection_online;
    const uint64_t previous_epoch = voice_input->status.connection_epoch;
    if (online) {
        voice_input->status.connection_online = true;
        voice_input->status.connection_epoch = connection_epoch;
    } else if (voice_input->status.connection_epoch == connection_epoch) {
        voice_input->status.connection_online = false;
    }
    taskEXIT_CRITICAL(&voice_input->status_lock);
    const voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_CONNECTION_CHANGED,
        .value.connection = {
            .online = online,
            .connection_epoch = connection_epoch,
        },
    };
    const voice_input_result_t result = control_enqueue(voice_input, &control);
    if (result != VOICE_INPUT_RESULT_OK) {
        taskENTER_CRITICAL(&voice_input->status_lock);
        voice_input->status.connection_online = previous_online;
        voice_input->status.connection_epoch = previous_epoch;
        taskEXIT_CRITICAL(&voice_input->status_lock);
    }
    return result;
}

voice_input_result_t voice_input_segment_accept(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const device_media_input_segment_accepted_t *accepted)
{
    if (voice_input == NULL || connection_epoch == 0U || accepted == NULL ||
        accepted->client_input_id[0] == '\0' ||
        memchr(
            accepted->client_input_id,
            '\0',
            sizeof(accepted->client_input_id)) == NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    const voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_SEGMENT_ACCEPTED,
        .value.segment_accepted = {
            .connection_epoch = connection_epoch,
            .accepted = *accepted,
        },
    };
    return control_enqueue(voice_input, &control);
}

voice_input_result_t voice_input_final_accept(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const device_media_input_accepted_t *accepted)
{
    if (voice_input == NULL || connection_epoch == 0U || accepted == NULL ||
        accepted->client_input_id[0] == '\0' ||
        memchr(accepted->client_input_id, '\0', sizeof(accepted->client_input_id)) == NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    const voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_FINAL_ACCEPTED,
        .value.final_accepted = {
            .connection_epoch = connection_epoch,
            .accepted = *accepted,
        },
    };
    return control_enqueue(voice_input, &control);
}

voice_input_result_t voice_input_reject(
    voice_input_t *voice_input,
    uint64_t connection_epoch,
    const char *client_input_id)
{
    if (voice_input == NULL || connection_epoch == 0U ||
        client_input_id == NULL || client_input_id[0] == '\0' ||
        memchr(client_input_id, '\0', DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U) == NULL) {
        return VOICE_INPUT_RESULT_INVALID_ARGUMENT;
    }
    voice_input_control_t control = {
        .type = VOICE_INPUT_CONTROL_REJECTED,
        .value.rejected.connection_epoch = connection_epoch,
    };
    memcpy(control.value.rejected.client_input_id, client_input_id,
           sizeof(control.value.rejected.client_input_id));
    return control_enqueue(voice_input, &control);
}

void voice_input_get_status(
    voice_input_t *voice_input,
    voice_input_status_t *status)
{
    if (voice_input == NULL || status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    *status = voice_input->status;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    status->queued_frame_count = uxQueueMessagesWaiting(voice_input->frame_queue);
}

void voice_input_stop(voice_input_t *voice_input)
{
    if (voice_input == NULL) {
        return;
    }
    taskENTER_CRITICAL(&voice_input->status_lock);
    if (!voice_input->started) {
        taskEXIT_CRITICAL(&voice_input->status_lock);
        return;
    }
    voice_input->stop_requested = true;
    TaskHandle_t task = voice_input->task;
    taskEXIT_CRITICAL(&voice_input->status_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    (void)xEventGroupWaitBits(
        voice_input->lifecycle_events,
        VOICE_INPUT_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void voice_input_destroy(voice_input_t *voice_input)
{
    if (voice_input == NULL) {
        return;
    }
    voice_input_stop(voice_input);
    utterance_cache_destroy(voice_input->cache);
    vQueueDelete(voice_input->frame_queue);
    vQueueDelete(voice_input->control_queue);
    free(voice_input->frame_queue_storage);
    free(voice_input->control_queue_storage);
    vEventGroupDelete(voice_input->lifecycle_events);
    memset(voice_input, 0, sizeof(*voice_input));
    free(voice_input);
}
