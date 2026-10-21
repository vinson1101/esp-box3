#include "device_authentication.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_base64url.h"
#include "device_ed25519.h"
#include "device_transcript.h"

#define AUTHENTICATION_TRANSCRIPT_MAX_BYTES 2048U
#define AUTHENTICATION_BASE64_SIGNATURE_BYTES 86U

static bool visible_string(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > maximum_size) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!isspace((unsigned char)value[index])) {
            return true;
        }
    }
    return false;
}

static bool payload_has_exact_fields(
    const cJSON *payload,
    const char *const *field_names,
    size_t field_count)
{
    if (!cJSON_IsObject(payload)) {
        return false;
    }
    bool found[8U] = {false};
    if (field_count > sizeof(found) / sizeof(found[0])) {
        return false;
    }
    size_t actual_count = 0U;
    for (const cJSON *field = payload->child; field != NULL; field = field->next) {
        if (field->string == NULL) {
            return false;
        }
        size_t matched_index = field_count;
        for (size_t index = 0U; index < field_count; ++index) {
            if (strcmp(field->string, field_names[index]) == 0) {
                matched_index = index;
                break;
            }
        }
        if (matched_index == field_count || found[matched_index]) {
            return false;
        }
        found[matched_index] = true;
        ++actual_count;
    }
    if (actual_count != field_count) {
        return false;
    }
    for (size_t index = 0U; index < field_count; ++index) {
        if (!found[index]) {
            return false;
        }
    }
    return true;
}

static const char *output_preference_name(device_output_preference_t preference)
{
    switch (preference) {
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

static bool parse_output_preference(
    const cJSON *value,
    device_output_preference_t *preference)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL) {
        return false;
    }
    if (strcmp(value->valuestring, "text") == 0) {
        *preference = DEVICE_OUTPUT_PREFERENCE_TEXT;
        return true;
    }
    if (strcmp(value->valuestring, "audio") == 0) {
        *preference = DEVICE_OUTPUT_PREFERENCE_AUDIO;
        return true;
    }
    if (strcmp(value->valuestring, "text_and_audio") == 0) {
        *preference = DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
        return true;
    }
    return false;
}

static bool add_capabilities(
    cJSON *payload,
    const device_capabilities_t *capabilities)
{
    cJSON *value = cJSON_CreateObject();
    if (value == NULL ||
        cJSON_AddBoolToObject(value, "input_text", capabilities->input_text) == NULL ||
        cJSON_AddBoolToObject(
            value,
            "input_pcm16_16k_mono",
            capabilities->input_pcm16_16k_mono) == NULL ||
        cJSON_AddBoolToObject(value, "output_text", capabilities->output_text) == NULL ||
        cJSON_AddBoolToObject(
            value,
            "output_pcm16_16k_mono",
            capabilities->output_pcm16_16k_mono) == NULL ||
        cJSON_AddBoolToObject(value, "playback_cancel", capabilities->playback_cancel) == NULL ||
        cJSON_AddBoolToObject(value, "display_status", capabilities->display_status) == NULL ||
        cJSON_AddBoolToObject(
            value,
            "display_transcript",
            capabilities->display_transcript) == NULL ||
        !cJSON_AddItemToObject(payload, "capabilities", value)) {
        cJSON_Delete(value);
        return false;
    }
    return true;
}

static bool parse_capabilities(
    const cJSON *value,
    device_capabilities_t *capabilities)
{
    static const char *const fields[] = {
        "input_text",
        "input_pcm16_16k_mono",
        "output_text",
        "output_pcm16_16k_mono",
        "playback_cancel",
        "display_status",
        "display_transcript",
    };
    if (!payload_has_exact_fields(
            value,
            fields,
            sizeof(fields) / sizeof(fields[0]))) {
        return false;
    }
    bool *outputs[] = {
        &capabilities->input_text,
        &capabilities->input_pcm16_16k_mono,
        &capabilities->output_text,
        &capabilities->output_pcm16_16k_mono,
        &capabilities->playback_cancel,
        &capabilities->display_status,
        &capabilities->display_transcript,
    };
    for (size_t index = 0U; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(value, fields[index]);
        if (!cJSON_IsBool(field)) {
            return false;
        }
        *outputs[index] = cJSON_IsTrue(field);
    }
    return true;
}

device_authentication_result_t device_authentication_decode_challenge(
    const device_control_envelope_t *envelope,
    device_authentication_challenge_t *challenge)
{
    static const char *const fields[] = {"connection_id", "nonce", "server_time_ms"};
    if (envelope == NULL || challenge == NULL) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "auth_challenge") != 0) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0]))) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(payload, "connection_id");
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
    const cJSON *server_time_ms = cJSON_GetObjectItemCaseSensitive(payload, "server_time_ms");
    if (!cJSON_IsString(connection_id) ||
        !visible_string(
            connection_id->valuestring,
            DEVICE_AUTHENTICATION_CONNECTION_ID_MAX_BYTES) ||
        !cJSON_IsString(nonce) ||
        !visible_string(nonce->valuestring, DEVICE_AUTHENTICATION_NONCE_MAX_BYTES) ||
        !cJSON_IsNumber(server_time_ms) || !isfinite(server_time_ms->valuedouble) ||
        server_time_ms->valuedouble < 0.0 ||
        server_time_ms->valuedouble > 9007199254740991.0 ||
        floor(server_time_ms->valuedouble) != server_time_ms->valuedouble) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    device_authentication_challenge_t decoded = {0};
    memcpy(
        decoded.connection_id,
        connection_id->valuestring,
        strlen(connection_id->valuestring) + 1U);
    memcpy(decoded.host_nonce, nonce->valuestring, strlen(nonce->valuestring) + 1U);
    decoded.server_time_ms = (int64_t)server_time_ms->valuedouble;
    *challenge = decoded;
    return DEVICE_AUTHENTICATION_RESULT_OK;
}

device_authentication_result_t device_authentication_encode_hello(
    const char *message_id,
    const device_authentication_challenge_t *challenge,
    const device_identity_record_t *identity,
    const char *device_nonce,
    const device_capabilities_t *capabilities,
    device_output_preference_t output_preference,
    const char *client_version,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    const char *preference_name = output_preference_name(output_preference);
    if (challenge == NULL || identity == NULL || capabilities == NULL || encoded_size == NULL ||
        identity->state == DEVICE_IDENTITY_STATE_NONE ||
        device_identity_record_validate(identity) != DEVICE_IDENTITY_RESULT_OK ||
        !visible_string(
            challenge->connection_id,
            DEVICE_AUTHENTICATION_CONNECTION_ID_MAX_BYTES) ||
        !visible_string(challenge->host_nonce, DEVICE_AUTHENTICATION_NONCE_MAX_BYTES) ||
        !visible_string(device_nonce, DEVICE_AUTHENTICATION_NONCE_MAX_BYTES) ||
        !visible_string(client_version, DEVICE_AUTHENTICATION_CLIENT_VERSION_MAX_BYTES) ||
        preference_name == NULL) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT;
    }
    uint8_t transcript[AUTHENTICATION_TRANSCRIPT_MAX_BYTES];
    size_t transcript_size = 0U;
    if (device_authentication_transcript_encode(
            challenge->connection_id,
            challenge->host_nonce,
            identity->device_id,
            device_nonce,
            capabilities,
            output_preference,
            transcript,
            sizeof(transcript),
            &transcript_size) != DEVICE_CRYPTO_RESULT_OK) {
        memset(transcript, 0, sizeof(transcript));
        return DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED;
    }
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES];
    if (device_ed25519_sign(
            identity->private_seed,
            transcript,
            transcript_size,
            signature) != DEVICE_CRYPTO_RESULT_OK) {
        memset(transcript, 0, sizeof(transcript));
        memset(signature, 0, sizeof(signature));
        return DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED;
    }
    char encoded_signature[AUTHENTICATION_BASE64_SIGNATURE_BYTES + 1U];
    size_t encoded_signature_size = 0U;
    if (device_base64url_encode(
            signature,
            sizeof(signature),
            encoded_signature,
            sizeof(encoded_signature) - 1U,
            &encoded_signature_size) != DEVICE_CRYPTO_RESULT_OK) {
        memset(transcript, 0, sizeof(transcript));
        memset(signature, 0, sizeof(signature));
        memset(encoded_signature, 0, sizeof(encoded_signature));
        return DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED;
    }
    encoded_signature[encoded_signature_size] = '\0';
    cJSON *payload = cJSON_CreateObject();
    bool payload_ready = payload != NULL &&
                         cJSON_AddStringToObject(payload, "device_id", identity->device_id) != NULL &&
                         cJSON_AddStringToObject(payload, "device_nonce", device_nonce) != NULL &&
                         add_capabilities(payload, capabilities) &&
                         cJSON_AddStringToObject(
                             payload,
                             "output_preference",
                             preference_name) != NULL &&
                         cJSON_AddStringToObject(payload, "client_version", client_version) != NULL &&
                         cJSON_AddStringToObject(payload, "signature", encoded_signature) != NULL;
    device_protocol_result_t protocol_result = DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    if (payload_ready) {
        protocol_result = device_control_envelope_encode(
            message_id,
            "hello",
            payload,
            encoded,
            encoded_capacity,
            encoded_size);
    }
    cJSON_Delete(payload);
    memset(transcript, 0, sizeof(transcript));
    memset(signature, 0, sizeof(signature));
    memset(encoded_signature, 0, sizeof(encoded_signature));
    if (!payload_ready) {
        return DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED;
    }
    if (protocol_result == DEVICE_PROTOCOL_RESULT_OUTPUT_TOO_SMALL) {
        return DEVICE_AUTHENTICATION_RESULT_OUTPUT_TOO_SMALL;
    }
    return protocol_result == DEVICE_PROTOCOL_RESULT_OK
        ? DEVICE_AUTHENTICATION_RESULT_OK
        : DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT;
}

device_authentication_result_t device_authentication_decode_ack(
    const device_control_envelope_t *envelope,
    const char *expected_device_id,
    const char *expected_connection_id,
    device_authentication_ack_t *ack)
{
    static const char *const fields[] = {
        "device_id", "connection_id", "capabilities", "output_preference"};
    if (envelope == NULL || expected_device_id == NULL ||
        expected_connection_id == NULL || ack == NULL) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "hello_ack") != 0) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0]))) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(payload, "device_id");
    const cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(payload, "connection_id");
    device_authentication_ack_t decoded = {0};
    if (!cJSON_IsString(device_id) || !cJSON_IsString(connection_id) ||
        strcmp(device_id->valuestring, expected_device_id) != 0 ||
        strcmp(connection_id->valuestring, expected_connection_id) != 0 ||
        !parse_capabilities(
            cJSON_GetObjectItemCaseSensitive(payload, "capabilities"),
            &decoded.capabilities) ||
        !parse_output_preference(
            cJSON_GetObjectItemCaseSensitive(payload, "output_preference"),
            &decoded.output_preference)) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    *ack = decoded;
    return DEVICE_AUTHENTICATION_RESULT_OK;
}

device_authentication_result_t device_authentication_decode_error(
    const device_control_envelope_t *envelope,
    device_authentication_error_t *error)
{
    static const char *const fields[] = {
        "code", "correlation_message_id", "recoverable"};
    if (envelope == NULL || error == NULL) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "error") != 0) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!payload_has_exact_fields(payload, fields, sizeof(fields) / sizeof(fields[0]))) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(payload, "code");
    const cJSON *correlation =
        cJSON_GetObjectItemCaseSensitive(payload, "correlation_message_id");
    const cJSON *recoverable = cJSON_GetObjectItemCaseSensitive(payload, "recoverable");
    if (!cJSON_IsString(code) ||
        !visible_string(code->valuestring, DEVICE_AUTHENTICATION_ERROR_CODE_MAX_BYTES) ||
        (!cJSON_IsNull(correlation) &&
         (!cJSON_IsString(correlation) ||
          !visible_string(correlation->valuestring, DEVICE_MESSAGE_ID_MAX_BYTES))) ||
        !cJSON_IsBool(recoverable)) {
        return DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE;
    }
    device_authentication_error_t decoded = {0};
    memcpy(decoded.code, code->valuestring, strlen(code->valuestring) + 1U);
    decoded.recoverable = cJSON_IsTrue(recoverable);
    if (cJSON_IsString(correlation)) {
        decoded.has_correlation_message_id = true;
        memcpy(
            decoded.correlation_message_id,
            correlation->valuestring,
            strlen(correlation->valuestring) + 1U);
    }
    *error = decoded;
    return DEVICE_AUTHENTICATION_RESULT_OK;
}
