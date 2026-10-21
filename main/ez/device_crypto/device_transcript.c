#include "device_transcript.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "device_protocol_version.h"

typedef struct {
    uint8_t *encoded;
    size_t capacity;
    size_t size;
    bool output_too_small;
    bool size_overflow;
} transcript_builder_t;

static void write_u64_be(uint8_t destination[8], uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static void transcript_builder_append(
    transcript_builder_t *builder,
    const uint8_t *part,
    size_t part_size)
{
    if (builder->size_overflow || (part_size > 0U && part == NULL)
            || part_size > SIZE_MAX - 8U
        || builder->size > SIZE_MAX - 8U - part_size) {
        builder->size_overflow = true;
        return;
    }

    const size_t next_size = builder->size + 8U + part_size;
    if (builder->encoded == NULL || next_size > builder->capacity) {
        builder->output_too_small = true;
    } else {
        write_u64_be(&builder->encoded[builder->size], (uint64_t)part_size);
        if (part_size > 0U) {
            memcpy(&builder->encoded[builder->size + 8U], part, part_size);
        }
    }
    builder->size = next_size;
}

static void transcript_builder_append_string(transcript_builder_t *builder, const char *part)
{
    if (part == NULL) {
        builder->size_overflow = true;
        return;
    }
    transcript_builder_append(builder, (const uint8_t *)part, strlen(part));
}

static device_crypto_result_t transcript_builder_finish(
    const transcript_builder_t *builder,
    size_t *encoded_size)
{
    if (encoded_size == NULL || builder->size_overflow) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    *encoded_size = builder->size;
    return builder->output_too_small ? DEVICE_CRYPTO_RESULT_OUTPUT_TOO_SMALL
                                     : DEVICE_CRYPTO_RESULT_OK;
}

static transcript_builder_t transcript_builder(uint8_t *encoded, size_t capacity)
{
    return (transcript_builder_t) {
        .encoded = encoded,
        .capacity = capacity,
    };
}

static bool pairing_code_is_valid(const char *pairing_code)
{
    if (pairing_code == NULL || strlen(pairing_code) != 6U) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        if (pairing_code[index] < '0' || pairing_code[index] > '9') {
            return false;
        }
    }
    return true;
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

device_crypto_result_t device_pairing_associated_data_encode(
    const char *pairing_request_id,
    const char *installation_id,
    const char *certificate_fingerprint,
    const char *device_nonce,
    const char *host_nonce,
    const device_capabilities_t *capabilities,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    uint8_t capability_bytes[DEVICE_CAPABILITIES_BYTES] = {0};
    if (device_capabilities_serialize(capabilities, capability_bytes) != DEVICE_PROTOCOL_RESULT_OK) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    transcript_builder_t builder = transcript_builder(encoded, encoded_capacity);
    transcript_builder_append_string(&builder, "ez-assistant-pairing-v1");
    transcript_builder_append_string(&builder, pairing_request_id);
    transcript_builder_append_string(&builder, installation_id);
    transcript_builder_append_string(&builder, certificate_fingerprint);
    transcript_builder_append_string(&builder, device_nonce);
    transcript_builder_append_string(&builder, host_nonce);
    transcript_builder_append(&builder, capability_bytes, sizeof(capability_bytes));
    return transcript_builder_finish(&builder, encoded_size);
}

device_crypto_result_t device_pairing_bind_transcript_encode(
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t public_key[32],
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (public_key == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    transcript_builder_t builder = transcript_builder(encoded, encoded_capacity);
    transcript_builder_append_string(&builder, "ez-assistant-bind-v1");
    transcript_builder_append(&builder, associated_data, associated_data_size);
    transcript_builder_append(&builder, public_key, 32U);
    return transcript_builder_finish(&builder, encoded_size);
}

device_crypto_result_t device_pairing_commit_transcript_encode(
    const uint8_t *bind_transcript,
    size_t bind_transcript_size,
    const char *device_id,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    transcript_builder_t builder = transcript_builder(encoded, encoded_capacity);
    transcript_builder_append_string(&builder, "ez-assistant-commit-v1");
    transcript_builder_append(&builder, bind_transcript, bind_transcript_size);
    transcript_builder_append_string(&builder, device_id);
    return transcript_builder_finish(&builder, encoded_size);
}

device_crypto_result_t device_authentication_transcript_encode(
    const char *connection_id,
    const char *host_nonce,
    const char *device_id,
    const char *device_nonce,
    const device_capabilities_t *capabilities,
    device_output_preference_t output_preference,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    uint8_t capability_bytes[DEVICE_CAPABILITIES_BYTES] = {0};
    const char *preference_name = output_preference_name(output_preference);
    if (preference_name == NULL
        || device_capabilities_serialize(capabilities, capability_bytes)
               != DEVICE_PROTOCOL_RESULT_OK) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    const uint8_t protocol_major[2] = {0U, DEVICE_PROTOCOL_MAJOR};
    const uint8_t protocol_minor[2] = {0U, DEVICE_PROTOCOL_MINOR};
    transcript_builder_t builder = transcript_builder(encoded, encoded_capacity);
    transcript_builder_append_string(&builder, "ez-assistant-auth-v1");
    transcript_builder_append(&builder, protocol_major, sizeof(protocol_major));
    transcript_builder_append(&builder, protocol_minor, sizeof(protocol_minor));
    transcript_builder_append_string(&builder, connection_id);
    transcript_builder_append_string(&builder, host_nonce);
    transcript_builder_append_string(&builder, device_id);
    transcript_builder_append_string(&builder, device_nonce);
    transcript_builder_append(&builder, capability_bytes, sizeof(capability_bytes));
    transcript_builder_append_string(&builder, preference_name);
    return transcript_builder_finish(&builder, encoded_size);
}

device_crypto_result_t device_pairing_password_transcript_encode(
    const char *pairing_code,
    const char *pairing_request_id,
    const char *installation_id,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (!pairing_code_is_valid(pairing_code)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    transcript_builder_t builder = transcript_builder(encoded, encoded_capacity);
    transcript_builder_append_string(&builder, "ez-assistant-spake2-password-v1");
    transcript_builder_append_string(&builder, pairing_code);
    transcript_builder_append_string(&builder, pairing_request_id);
    transcript_builder_append_string(&builder, installation_id);
    return transcript_builder_finish(&builder, encoded_size);
}
