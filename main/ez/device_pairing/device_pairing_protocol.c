#include "device_pairing_protocol.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "device_base64url.h"

#define DEVICE_PAIRING_SHARE_BASE64URL_BYTES 87U
#define DEVICE_PAIRING_PUBLIC_KEY_BASE64URL_BYTES 43U
#define DEVICE_PAIRING_SIGNATURE_BASE64URL_BYTES 86U
#define DEVICE_PAIRING_MAC_BASE64URL_BYTES 43U

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
        const unsigned char character = (unsigned char)value[index];
        if (character < 0x20U || character == 0x7FU) {
            return false;
        }
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
    if (!cJSON_IsObject(payload) || field_count > 8U) {
        return false;
    }
    bool found[8U] = {false};
    size_t actual_count = 0U;
    for (const cJSON *field = payload->child; field != NULL; field = field->next) {
        if (field->string == NULL) {
            return false;
        }
        size_t matched = field_count;
        for (size_t index = 0U; index < field_count; ++index) {
            if (strcmp(field->string, field_names[index]) == 0) {
                matched = index;
                break;
            }
        }
        if (matched == field_count || found[matched]) {
            return false;
        }
        found[matched] = true;
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

static bool add_capabilities(cJSON *payload, const device_capabilities_t *capabilities)
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

static device_pairing_protocol_result_t encode_binary(
    const uint8_t *value,
    size_t value_size,
    char *encoded,
    size_t encoded_capacity)
{
    size_t encoded_size = 0U;
    if (value == NULL || encoded == NULL || encoded_capacity == 0U ||
        device_base64url_encode(
            value,
            value_size,
            encoded,
            encoded_capacity - 1U,
            &encoded_size) != DEVICE_CRYPTO_RESULT_OK ||
        encoded_size >= encoded_capacity) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    encoded[encoded_size] = '\0';
    return DEVICE_PAIRING_PROTOCOL_RESULT_OK;
}

static bool decode_binary(
    const cJSON *value,
    uint8_t *decoded,
    size_t expected_size)
{
    size_t decoded_size = 0U;
    return cJSON_IsString(value) && value->valuestring != NULL &&
           device_base64url_decode(
               value->valuestring,
               strlen(value->valuestring),
               decoded,
               expected_size,
               &decoded_size) == DEVICE_CRYPTO_RESULT_OK &&
           decoded_size == expected_size;
}

static device_pairing_protocol_result_t encode_payload(
    const char *message_id,
    const char *message_type,
    cJSON *payload,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (payload == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    const device_protocol_result_t result = device_control_envelope_encode(
        message_id,
        message_type,
        payload,
        encoded,
        encoded_capacity,
        encoded_size);
    if (result == DEVICE_PROTOCOL_RESULT_OUTPUT_TOO_SMALL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_OUTPUT_TOO_SMALL;
    }
    return result == DEVICE_PROTOCOL_RESULT_OK
        ? DEVICE_PAIRING_PROTOCOL_RESULT_OK
        : DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
}

device_pairing_protocol_result_t device_pairing_encode_hello(
    const char *message_id,
    const char *pairing_request_id,
    const char *display_name,
    const char *device_nonce,
    const device_capabilities_t *capabilities,
    const uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (!visible_string(pairing_request_id, DEVICE_PAIRING_REQUEST_ID_MAX_BYTES) ||
        !visible_string(display_name, DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES) ||
        !visible_string(device_nonce, DEVICE_PAIRING_NONCE_MAX_BYTES) || capabilities == NULL ||
        device_share == NULL || encoded_size == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    char share[DEVICE_PAIRING_SHARE_BASE64URL_BYTES + 1U];
    if (encode_binary(device_share, DEVICE_SPAKE2_SHARE_BYTES, share, sizeof(share)) !=
        DEVICE_PAIRING_PROTOCOL_RESULT_OK) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    cJSON *payload = cJSON_CreateObject();
    const bool ready = payload != NULL &&
                       cJSON_AddStringToObject(
                           payload,
                           "pairing_request_id",
                           pairing_request_id) != NULL &&
                       cJSON_AddStringToObject(payload, "display_name", display_name) != NULL &&
                       cJSON_AddStringToObject(payload, "device_nonce", device_nonce) != NULL &&
                       add_capabilities(payload, capabilities) &&
                       cJSON_AddStringToObject(payload, "pake_share", share) != NULL;
    const device_pairing_protocol_result_t result = ready
        ? encode_payload(
              message_id,
              "pairing_hello",
              payload,
              encoded,
              encoded_capacity,
              encoded_size)
        : DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    cJSON_Delete(payload);
    memset(share, 0, sizeof(share));
    return result;
}

device_pairing_protocol_result_t device_pairing_decode_pending(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_pending_t *pending)
{
    static const char *const fields[] = {"pairing_request_id", "expires_at_ms"};
    if (envelope == NULL || expected_pairing_request_id == NULL || pending == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "pairing_pending") != 0) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(payload, "pairing_request_id");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(payload, "expires_at_ms");
    if (!payload_has_exact_fields(payload, fields, 2U) || !cJSON_IsString(request) ||
        strcmp(request->valuestring, expected_pairing_request_id) != 0 ||
        !cJSON_IsNumber(expires) || !isfinite(expires->valuedouble) ||
        expires->valuedouble <= 0.0 || expires->valuedouble > 9007199254740991.0 ||
        floor(expires->valuedouble) != expires->valuedouble) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    pending->expires_at_ms = (int64_t)expires->valuedouble;
    return DEVICE_PAIRING_PROTOCOL_RESULT_OK;
}

device_pairing_protocol_result_t device_pairing_decode_pake(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_pake_t *pake)
{
    static const char *const fields[] = {
        "pairing_request_id", "host_nonce", "pake_share", "confirmation_mac"};
    if (envelope == NULL || expected_pairing_request_id == NULL || pake == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "pairing_pake") != 0) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(payload, "pairing_request_id");
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "host_nonce");
    device_pairing_pake_t decoded = {0};
    if (!payload_has_exact_fields(payload, fields, 4U) || !cJSON_IsString(request) ||
        strcmp(request->valuestring, expected_pairing_request_id) != 0 ||
        !cJSON_IsString(nonce) ||
        !visible_string(nonce->valuestring, DEVICE_PAIRING_NONCE_MAX_BYTES) ||
        !decode_binary(
            cJSON_GetObjectItemCaseSensitive(payload, "pake_share"),
            decoded.host_share,
            sizeof(decoded.host_share)) ||
        !decode_binary(
            cJSON_GetObjectItemCaseSensitive(payload, "confirmation_mac"),
            decoded.host_confirmation,
            sizeof(decoded.host_confirmation))) {
        memset(&decoded, 0, sizeof(decoded));
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    memcpy(decoded.host_nonce, nonce->valuestring, strlen(nonce->valuestring) + 1U);
    *pake = decoded;
    return DEVICE_PAIRING_PROTOCOL_RESULT_OK;
}

device_pairing_protocol_result_t device_pairing_encode_confirmation(
    const char *message_id,
    const char *pairing_request_id,
    const uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (!visible_string(pairing_request_id, DEVICE_PAIRING_REQUEST_ID_MAX_BYTES) ||
        confirmation == NULL || encoded_size == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    char value[DEVICE_PAIRING_MAC_BASE64URL_BYTES + 1U];
    if (encode_binary(confirmation, DEVICE_SPAKE2_CONFIRMATION_BYTES, value, sizeof(value)) !=
        DEVICE_PAIRING_PROTOCOL_RESULT_OK) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    cJSON *payload = cJSON_CreateObject();
    const bool ready = payload != NULL &&
                       cJSON_AddStringToObject(
                           payload,
                           "pairing_request_id",
                           pairing_request_id) != NULL &&
                       cJSON_AddStringToObject(payload, "confirmation_mac", value) != NULL;
    const device_pairing_protocol_result_t result = ready
        ? encode_payload(
              message_id,
              "pairing_confirm",
              payload,
              encoded,
              encoded_capacity,
              encoded_size)
        : DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    cJSON_Delete(payload);
    memset(value, 0, sizeof(value));
    return result;
}

device_pairing_protocol_result_t device_pairing_encode_bind(
    const char *message_id,
    const char *pairing_request_id,
    const uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES],
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (!visible_string(pairing_request_id, DEVICE_PAIRING_REQUEST_ID_MAX_BYTES) ||
        public_key == NULL || signature == NULL || binding_mac == NULL || encoded_size == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    char key[DEVICE_PAIRING_PUBLIC_KEY_BASE64URL_BYTES + 1U];
    char signature_value[DEVICE_PAIRING_SIGNATURE_BASE64URL_BYTES + 1U];
    char mac[DEVICE_PAIRING_MAC_BASE64URL_BYTES + 1U];
    if (encode_binary(public_key, DEVICE_ED25519_PUBLIC_KEY_BYTES, key, sizeof(key)) !=
            DEVICE_PAIRING_PROTOCOL_RESULT_OK ||
        encode_binary(signature, DEVICE_ED25519_SIGNATURE_BYTES, signature_value, sizeof(signature_value)) !=
            DEVICE_PAIRING_PROTOCOL_RESULT_OK ||
        encode_binary(binding_mac, DEVICE_SPAKE2_BINDING_MAC_BYTES, mac, sizeof(mac)) !=
            DEVICE_PAIRING_PROTOCOL_RESULT_OK) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    cJSON *payload = cJSON_CreateObject();
    const bool ready = payload != NULL &&
                       cJSON_AddStringToObject(
                           payload,
                           "pairing_request_id",
                           pairing_request_id) != NULL &&
                       cJSON_AddStringToObject(payload, "public_key", key) != NULL &&
                       cJSON_AddStringToObject(payload, "signature", signature_value) != NULL &&
                       cJSON_AddStringToObject(payload, "binding_mac", mac) != NULL;
    const device_pairing_protocol_result_t result = ready
        ? encode_payload(
              message_id,
              "pairing_bind",
              payload,
              encoded,
              encoded_capacity,
              encoded_size)
        : DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    cJSON_Delete(payload);
    memset(key, 0, sizeof(key));
    memset(signature_value, 0, sizeof(signature_value));
    memset(mac, 0, sizeof(mac));
    return result;
}

device_pairing_protocol_result_t device_pairing_decode_bind_ack(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_bind_ack_t *ack)
{
    static const char *const fields[] = {"pairing_request_id", "device_id", "host_proof"};
    if (envelope == NULL || expected_pairing_request_id == NULL || ack == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "pairing_bind_ack") != 0) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(payload, "pairing_request_id");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(payload, "device_id");
    device_pairing_bind_ack_t decoded = {0};
    if (!payload_has_exact_fields(payload, fields, 3U) || !cJSON_IsString(request) ||
        strcmp(request->valuestring, expected_pairing_request_id) != 0 ||
        !cJSON_IsString(device_id) ||
        !visible_string(device_id->valuestring, DEVICE_PAIRING_DEVICE_ID_MAX_BYTES) ||
        !decode_binary(
            cJSON_GetObjectItemCaseSensitive(payload, "host_proof"),
            decoded.host_proof,
            sizeof(decoded.host_proof))) {
        memset(&decoded, 0, sizeof(decoded));
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    memcpy(decoded.device_id, device_id->valuestring, strlen(device_id->valuestring) + 1U);
    *ack = decoded;
    return DEVICE_PAIRING_PROTOCOL_RESULT_OK;
}

device_pairing_protocol_result_t device_pairing_encode_commit(
    const char *message_id,
    const char *pairing_request_id,
    const char *device_id,
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES],
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (!visible_string(pairing_request_id, DEVICE_PAIRING_REQUEST_ID_MAX_BYTES) ||
        !visible_string(device_id, DEVICE_PAIRING_DEVICE_ID_MAX_BYTES) || signature == NULL ||
        binding_mac == NULL || encoded_size == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    char signature_value[DEVICE_PAIRING_SIGNATURE_BASE64URL_BYTES + 1U];
    char mac[DEVICE_PAIRING_MAC_BASE64URL_BYTES + 1U];
    if (encode_binary(signature, DEVICE_ED25519_SIGNATURE_BYTES, signature_value, sizeof(signature_value)) !=
            DEVICE_PAIRING_PROTOCOL_RESULT_OK ||
        encode_binary(binding_mac, DEVICE_SPAKE2_BINDING_MAC_BYTES, mac, sizeof(mac)) !=
            DEVICE_PAIRING_PROTOCOL_RESULT_OK) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    }
    cJSON *payload = cJSON_CreateObject();
    const bool ready = payload != NULL &&
                       cJSON_AddStringToObject(
                           payload,
                           "pairing_request_id",
                           pairing_request_id) != NULL &&
                       cJSON_AddStringToObject(payload, "device_id", device_id) != NULL &&
                       cJSON_AddStringToObject(payload, "signature", signature_value) != NULL &&
                       cJSON_AddStringToObject(payload, "binding_mac", mac) != NULL;
    const device_pairing_protocol_result_t result = ready
        ? encode_payload(
              message_id,
              "pairing_commit",
              payload,
              encoded,
              encoded_capacity,
              encoded_size)
        : DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED;
    cJSON_Delete(payload);
    memset(signature_value, 0, sizeof(signature_value));
    memset(mac, 0, sizeof(mac));
    return result;
}

device_pairing_protocol_result_t device_pairing_decode_complete(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    const char *expected_device_id,
    device_pairing_complete_t *complete)
{
    static const char *const fields[] = {"pairing_request_id", "device_id", "display_name"};
    if (envelope == NULL || expected_pairing_request_id == NULL || expected_device_id == NULL ||
        complete == NULL) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (strcmp(device_control_envelope_message_type(envelope), "pairing_complete") != 0) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    const cJSON *payload = device_control_envelope_payload(envelope);
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(payload, "pairing_request_id");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(payload, "device_id");
    const cJSON *display_name = cJSON_GetObjectItemCaseSensitive(payload, "display_name");
    if (!payload_has_exact_fields(payload, fields, 3U) || !cJSON_IsString(request) ||
        strcmp(request->valuestring, expected_pairing_request_id) != 0 ||
        !cJSON_IsString(device_id) || strcmp(device_id->valuestring, expected_device_id) != 0 ||
        !cJSON_IsString(display_name) ||
        !visible_string(display_name->valuestring, DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES)) {
        return DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE;
    }
    memset(complete, 0, sizeof(*complete));
    memcpy(
        complete->display_name,
        display_name->valuestring,
        strlen(display_name->valuestring) + 1U);
    return DEVICE_PAIRING_PROTOCOL_RESULT_OK;
}
