#include "device_media_control.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "device_control_envelope.h"

#define DEVICE_MEDIA_MAX_PAYLOAD_FIELDS 6U
#define DEVICE_MEDIA_MAX_SAFE_INTEGER 9007199254740991.0

static bool identifier_is_valid(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t value_size = strlen(value);
    if (value_size == 0U || value_size > maximum_size) {
        return false;
    }
    for (size_t index = 0U; index < value_size; ++index) {
        if (!isspace((unsigned char)value[index])) {
            return true;
        }
    }
    return false;
}

static bool payload_has_exact_fields(const cJSON *payload, const char *const *field_names,
                                     size_t field_count)
{
    if (!cJSON_IsObject(payload) || field_names == NULL || field_count == 0U ||
        field_count > DEVICE_MEDIA_MAX_PAYLOAD_FIELDS) {
        return false;
    }
    bool present[DEVICE_MEDIA_MAX_PAYLOAD_FIELDS] = {false};
    size_t actual_count = 0U;
    for (const cJSON *field = payload->child; field != NULL; field = field->next) {
        if (field->string == NULL) {
            return false;
        }
        size_t index = 0U;
        while (index < field_count && strcmp(field->string, field_names[index]) != 0) {
            ++index;
        }
        if (index == field_count || present[index]) {
            return false;
        }
        present[index] = true;
        ++actual_count;
    }
    if (actual_count != field_count) {
        return false;
    }
    for (size_t index = 0U; index < field_count; ++index) {
        if (!present[index]) {
            return false;
        }
    }
    return true;
}

static bool copy_required_identifier(const cJSON *value, char *destination, size_t maximum_size)
{
    if (!cJSON_IsString(value) || !identifier_is_valid(value->valuestring, maximum_size)) {
        return false;
    }
    memcpy(destination, value->valuestring, strlen(value->valuestring) + 1U);
    return true;
}

static bool text_is_valid(const cJSON* value) {
    if (!cJSON_IsString(value) ||
        !identifier_is_valid(value->valuestring, DEVICE_MEDIA_TEXT_MAX_BYTES)) {
        return false;
    }
    const unsigned char* text = (const unsigned char*)value->valuestring;
    const size_t size = strlen(value->valuestring);
    for (size_t i = 0; i < size;) {
        uint32_t point = text[i++];
        unsigned extra = 0;
        uint32_t minimum = 0;
        if (point >= 0xc2 && point <= 0xdf) {
            extra = 1;
            minimum = 0x80;
            point &= 0x1f;
        } else if (point >= 0xe0 && point <= 0xef) {
            extra = 2;
            minimum = 0x800;
            point &= 0x0f;
        } else if (point >= 0xf0 && point <= 0xf4) {
            extra = 3;
            minimum = 0x10000;
            point &= 7;
        } else if (point >= 0x80 ||
                   (point < 0x20 && point != '\n' && point != '\r' && point != '\t')) {
            return false;
        }
        if (i + extra > size) {
            return false;
        }
        for (unsigned j = 0; j < extra; ++j) {
            if ((text[i] & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (text[i++] & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static bool copy_optional_identifier(const cJSON *value, char *destination, size_t maximum_size,
                                     bool *present)
{
    if (cJSON_IsNull(value)) {
        *present = false;
        return true;
    }
    if (!copy_required_identifier(value, destination, maximum_size)) {
        return false;
    }
    *present = true;
    return true;
}

static bool number_is_u32(const cJSON *value, bool allow_zero, uint32_t *parsed)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0.0 ||
        value->valuedouble > (double)UINT32_MAX ||
        floor(value->valuedouble) != value->valuedouble ||
        (!allow_zero && value->valuedouble == 0.0)) {
        return false;
    }
    *parsed = (uint32_t)value->valuedouble;
    return true;
}

static bool number_is_positive_safe_u64(const cJSON *value, uint64_t *parsed)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble <= 0.0 ||
        value->valuedouble > DEVICE_MEDIA_MAX_SAFE_INTEGER ||
        floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *parsed = (uint64_t)value->valuedouble;
    return true;
}

static bool pcm_format_is_protocol_v1(const cJSON *format)
{
    static const char *const fields[] = {"encoding", "sample_rate_hz", "channels",
                                         "frame_duration_ms"};
    if (!payload_has_exact_fields(format, fields, sizeof(fields) / sizeof(fields[0]))) {
        return false;
    }
    const cJSON *encoding = cJSON_GetObjectItemCaseSensitive(format, "encoding");
    uint32_t sample_rate_hz = 0U;
    uint32_t channels = 0U;
    uint32_t frame_duration_ms = 0U;
    return cJSON_IsString(encoding) && strcmp(encoding->valuestring, "pcm_s16le") == 0 &&
           number_is_u32(cJSON_GetObjectItemCaseSensitive(format, "sample_rate_hz"), false,
                         &sample_rate_hz) &&
           sample_rate_hz == 16000U &&
           number_is_u32(cJSON_GetObjectItemCaseSensitive(format, "channels"), false, &channels) &&
           channels == 1U &&
           number_is_u32(cJSON_GetObjectItemCaseSensitive(format, "frame_duration_ms"), false,
                         &frame_duration_ms) &&
           frame_duration_ms == 20U;
}

static bool run_status_parse(const cJSON *value, device_media_run_status_t *status)
{
    static const struct {
        const char *name;
        device_media_run_status_t status;
    } mappings[] = {
        {"accepted", DEVICE_MEDIA_RUN_STATUS_ACCEPTED},
        {"running", DEVICE_MEDIA_RUN_STATUS_RUNNING},
        {"cancelling", DEVICE_MEDIA_RUN_STATUS_CANCELLING},
        {"completed", DEVICE_MEDIA_RUN_STATUS_COMPLETED},
        {"failed", DEVICE_MEDIA_RUN_STATUS_FAILED},
        {"cancelled", DEVICE_MEDIA_RUN_STATUS_CANCELLED},
        {"interrupted", DEVICE_MEDIA_RUN_STATUS_INTERRUPTED},
        {"compaction_required", DEVICE_MEDIA_RUN_STATUS_COMPACTION_REQUIRED},
    };
    if (!cJSON_IsString(value)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(mappings) / sizeof(mappings[0]); ++index) {
        if (strcmp(value->valuestring, mappings[index].name) == 0) {
            *status = mappings[index].status;
            return true;
        }
    }
    return false;
}

static bool interaction_state_parse(const cJSON *value, device_media_interaction_state_t *state)
{
    static const struct {
        const char *name;
        device_media_interaction_state_t state;
    } mappings[] = {
        {"idle", DEVICE_MEDIA_INTERACTION_STATE_IDLE},
        {"listening", DEVICE_MEDIA_INTERACTION_STATE_LISTENING},
        {"recognizing", DEVICE_MEDIA_INTERACTION_STATE_RECOGNIZING},
        {"processing", DEVICE_MEDIA_INTERACTION_STATE_PROCESSING},
        {"speaking", DEVICE_MEDIA_INTERACTION_STATE_SPEAKING},
        {"needs_attention", DEVICE_MEDIA_INTERACTION_STATE_NEEDS_ATTENTION},
        {"error", DEVICE_MEDIA_INTERACTION_STATE_ERROR},
        {"unavailable", DEVICE_MEDIA_INTERACTION_STATE_UNAVAILABLE},
    };
    if (!cJSON_IsString(value)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(mappings) / sizeof(mappings[0]); ++index) {
        if (strcmp(value->valuestring, mappings[index].name) == 0) {
            *state = mappings[index].state;
            return true;
        }
    }
    return false;
}

static const char *output_preference_name(device_output_preference_t output_preference)
{
    switch (output_preference) {
    case DEVICE_OUTPUT_PREFERENCE_TEXT:
        return "text";
    case DEVICE_OUTPUT_PREFERENCE_AUDIO:
        return "audio";
    case DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO:
        return "text_and_audio";
    default:
        return NULL;
    }
}

static device_protocol_result_t encode(const char *message_id, const char *message_type,
                                       cJSON *payload, char *encoded, size_t encoded_capacity,
                                       size_t *encoded_size)
{
    if (payload == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    const device_protocol_result_t result = device_control_envelope_encode(
        message_id, message_type, payload, encoded, encoded_capacity, encoded_size);
    cJSON_Delete(payload);
    return result;
}

device_protocol_result_t device_media_control_encode_listen_start(
    const char *message_id, const char *client_input_id, uint32_t stream_id,
    device_output_preference_t output_preference, char *encoded, size_t encoded_capacity,
    size_t *encoded_size)
{
    const char *preference = output_preference_name(output_preference);
    if (!identifier_is_valid(client_input_id, DEVICE_CLIENT_INPUT_ID_MAX_BYTES) ||
        stream_id == 0U || preference == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON *format = cJSON_CreateObject();
    if (payload == NULL || format == NULL ||
        cJSON_AddStringToObject(payload, "client_input_id", client_input_id) == NULL ||
        cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL ||
        cJSON_AddStringToObject(format, "encoding", "pcm_s16le") == NULL ||
        cJSON_AddNumberToObject(format, "sample_rate_hz", 16000U) == NULL ||
        cJSON_AddNumberToObject(format, "channels", 1U) == NULL ||
        cJSON_AddNumberToObject(format, "frame_duration_ms", 20U) == NULL) {
        cJSON_Delete(format);
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (!cJSON_AddItemToObject(payload, "format", format)) {
        cJSON_Delete(format);
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    format = NULL;
    if (cJSON_AddStringToObject(payload, "output_preference", preference) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_start", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_listen_stop(const char *message_id, uint32_t stream_id,
                                        uint32_t last_sequence, char *encoded,
                                        size_t encoded_capacity, size_t *encoded_size)
{
    if (stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL ||
        cJSON_AddNumberToObject(payload, "last_sequence", last_sequence) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_stop", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_listen_cancel(const char *message_id, uint32_t stream_id, char *encoded,
                                          size_t encoded_capacity, size_t *encoded_size)
{
    if (stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_cancel", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_playback_cancel(const char *message_id, const char *output_id,
                                            uint32_t stream_id, char *encoded,
                                            size_t encoded_capacity, size_t *encoded_size)
{
    if (!identifier_is_valid(output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES) || stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddStringToObject(payload, "output_id", output_id) == NULL ||
        cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "playback_cancel", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_decode_input_accepted(const device_control_envelope_t *envelope,
                                           device_media_input_accepted_t *accepted)
{
    static const char *const fields[] = {"client_input_id", "input_id", "run_id", "queue_state"};
    if (envelope == NULL || accepted == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "input_accepted") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(accepted, 0, sizeof(*accepted));
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0])) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "client_input_id"),
                                  accepted->client_input_id, DEVICE_CLIENT_INPUT_ID_MAX_BYTES) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "input_id"),
                                  accepted->input_id, DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "run_id"),
                                  accepted->run_id, DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES) ||
        !run_status_parse(cJSON_GetObjectItemCaseSensitive(payload, "queue_state"),
                          &accepted->queue_state)) {
        memset(accepted, 0, sizeof(*accepted));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_media_control_decode_input_segment_accepted(
    const device_control_envelope_t *envelope,
    device_media_input_segment_accepted_t *accepted)
{
    static const char *const fields[] = {"client_input_id", "stream_id"};
    if (envelope == NULL || accepted == NULL ||
        strcmp(device_control_envelope_message_type(envelope),
               "input_segment_accepted") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(accepted, 0, sizeof(*accepted));
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *stream_id = cJSON_GetObjectItemCaseSensitive(payload, "stream_id");
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0])) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "client_input_id"),
                                  accepted->client_input_id,
                                  DEVICE_CLIENT_INPUT_ID_MAX_BYTES) ||
        !cJSON_IsNumber(stream_id) || stream_id->valuedouble < 1.0 ||
        stream_id->valuedouble > (double)UINT32_MAX ||
        stream_id->valuedouble != (double)(uint32_t)stream_id->valuedouble) {
        memset(accepted, 0, sizeof(*accepted));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    accepted->stream_id = (uint32_t)stream_id->valuedouble;
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t
device_media_control_decode_state_changed(const device_control_envelope_t *envelope,
                                          device_media_state_changed_t *state_changed)
{
    static const char *const fields[] = {"run_id", "client_input_id", "state", "reason"};
    if (envelope == NULL || state_changed == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "state_changed") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(state_changed, 0, sizeof(*state_changed));
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(payload, "reason");
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0])) ||
        !copy_optional_identifier(cJSON_GetObjectItemCaseSensitive(payload, "run_id"),
                                  state_changed->run_id, DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES,
                                  &state_changed->has_run_id) ||
        !copy_optional_identifier(cJSON_GetObjectItemCaseSensitive(payload, "client_input_id"),
                                  state_changed->client_input_id, DEVICE_CLIENT_INPUT_ID_MAX_BYTES,
                                  &state_changed->has_client_input_id) ||
        !interaction_state_parse(cJSON_GetObjectItemCaseSensitive(payload, "state"),
                                 &state_changed->state) ||
        !copy_optional_identifier(reason, state_changed->reason, DEVICE_MEDIA_REASON_MAX_BYTES,
                                  &state_changed->has_reason)) {
        memset(state_changed, 0, sizeof(*state_changed));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t
device_media_control_decode_playback_start(const device_control_envelope_t *envelope,
                                           device_media_playback_start_t *playback_start)
{
    static const char *const fields[] = {"output_id", "run_id", "stream_id",
                                         "format",    "text",   "sample_count"};
    if (envelope == NULL || playback_start == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "playback_start") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(playback_start, 0, sizeof(*playback_start));
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0])) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "output_id"),
                                  playback_start->output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "run_id"),
                                  playback_start->run_id, DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES) ||
        !number_is_u32(cJSON_GetObjectItemCaseSensitive(payload, "stream_id"), false,
                       &playback_start->stream_id) ||
        !pcm_format_is_protocol_v1(cJSON_GetObjectItemCaseSensitive(payload, "format")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(payload, "text")) ||
        !number_is_positive_safe_u64(cJSON_GetObjectItemCaseSensitive(payload, "sample_count"),
                                     &playback_start->sample_count)) {
        memset(playback_start, 0, sizeof(*playback_start));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t
device_media_control_decode_playback_end(const device_control_envelope_t *envelope,
                                         device_media_playback_end_t *playback_end)
{
    static const char *const fields[] = {"output_id", "stream_id", "reason"};
    if (envelope == NULL || playback_end == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "playback_end") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(playback_end, 0, sizeof(*playback_end));
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0])) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "output_id"),
                                  playback_end->output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES) ||
        !number_is_u32(cJSON_GetObjectItemCaseSensitive(payload, "stream_id"), false,
                       &playback_end->stream_id) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "reason"),
                                  playback_end->reason, DEVICE_MEDIA_REASON_MAX_BYTES)) {
        memset(playback_end, 0, sizeof(*playback_end));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_media_control_decode_transcript(
    const device_control_envelope_t* envelope, device_media_transcript_t* transcript) {
    static const char* const fields[] = {"client_input_id", "text"};
    if (envelope == NULL || transcript == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "transcript") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(transcript, 0, sizeof(*transcript));
    const cJSON* payload = device_control_envelope_payload(envelope);
    const cJSON* text = cJSON_GetObjectItemCaseSensitive(payload, "text");
    if (!payload_has_exact_fields(payload, fields, 2) || !text_is_valid(text) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "client_input_id"),
                                  transcript->client_input_id, DEVICE_CLIENT_INPUT_ID_MAX_BYTES)) {
        memset(transcript, 0, sizeof(*transcript));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    transcript->text = text->valuestring;
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_media_control_decode_text_output(
    const device_control_envelope_t* envelope, device_media_text_output_t* output) {
    static const char* const fields[] = {"output_id", "run_id", "text"};
    if (envelope == NULL || output == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "text_output") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(output, 0, sizeof(*output));
    const cJSON* payload = device_control_envelope_payload(envelope);
    const cJSON* text = cJSON_GetObjectItemCaseSensitive(payload, "text");
    if (!payload_has_exact_fields(payload, fields, 3) || !text_is_valid(text) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "output_id"),
                                  output->output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES) ||
        !copy_required_identifier(cJSON_GetObjectItemCaseSensitive(payload, "run_id"),
                                  output->run_id, DEVICE_MEDIA_RUNTIME_ID_MAX_BYTES)) {
        memset(output, 0, sizeof(*output));
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    output->text = text->valuestring;
    return DEVICE_PROTOCOL_RESULT_OK;
}
