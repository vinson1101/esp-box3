#include "device_identity_record.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RECORD_MAGIC 0x31495A45UL
#define RECORD_HEADER_BYTES 16U
#define RECORD_DEVICE_ID_OFFSET RECORD_HEADER_BYTES
#define RECORD_INSTALLATION_ID_OFFSET \
    (RECORD_DEVICE_ID_OFFSET + DEVICE_IDENTITY_DEVICE_ID_MAX_BYTES + 1U)
#define RECORD_FINGERPRINT_OFFSET \
    (RECORD_INSTALLATION_ID_OFFSET + HOST_DISCOVERY_INSTALLATION_ID_LENGTH + 1U)
#define RECORD_PRIVATE_SEED_OFFSET \
    (RECORD_FINGERPRINT_OFFSET + HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH + 1U)
#define RECORD_PUBLIC_KEY_OFFSET \
    (RECORD_PRIVATE_SEED_OFFSET + DEVICE_ED25519_SEED_BYTES)

_Static_assert(
    RECORD_PUBLIC_KEY_OFFSET + DEVICE_ED25519_PUBLIC_KEY_BYTES ==
        DEVICE_IDENTITY_RECORD_ENCODED_BYTES,
    "device identity record size is inconsistent");

static bool bytes_are_zero(const uint8_t *bytes, size_t byte_count)
{
    uint8_t combined = 0U;
    for (size_t index = 0U; index < byte_count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

static bool bounded_string_length(
    const char *value,
    size_t capacity,
    size_t *length)
{
    if (value == NULL || capacity == 0U || length == NULL) {
        return false;
    }
    for (size_t index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') {
            *length = index;
            return true;
        }
    }
    return false;
}

static bool is_lower_hex_string(
    const char *value,
    size_t capacity,
    size_t expected_length)
{
    size_t length = 0U;
    if (!bounded_string_length(value, capacity, &length) || length != expected_length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool device_id_is_valid(const char *device_id)
{
    size_t length = 0U;
    if (!bounded_string_length(
            device_id,
            DEVICE_IDENTITY_DEVICE_ID_MAX_BYTES + 1U,
            &length) ||
        length == 0U) {
        return false;
    }
    size_t first = 0U;
    while (first < length &&
           (device_id[first] == ' ' || device_id[first] == '\t' ||
            device_id[first] == '\r' || device_id[first] == '\n')) {
        ++first;
    }
    if (first == length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char character = (unsigned char)device_id[index];
        if (character < 0x20U || character == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool host_binding_is_empty(const device_identity_record_t *record)
{
    return record->host_installation_id[0] == '\0' &&
           record->host_certificate_fingerprint[0] == '\0';
}

static bool host_binding_is_valid(const device_identity_record_t *record)
{
    return is_lower_hex_string(
               record->host_installation_id,
               sizeof(record->host_installation_id),
               HOST_DISCOVERY_INSTALLATION_ID_LENGTH) &&
           is_lower_hex_string(
               record->host_certificate_fingerprint,
               sizeof(record->host_certificate_fingerprint),
               HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH);
}

device_identity_result_t device_identity_record_validate(
    const device_identity_record_t *record)
{
    if (record == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    if (record->state == DEVICE_IDENTITY_STATE_NONE) {
        if (record->device_id[0] != '\0' ||
            !bytes_are_zero(record->private_seed, sizeof(record->private_seed)) ||
            !bytes_are_zero(record->public_key, sizeof(record->public_key))) {
            return DEVICE_IDENTITY_RESULT_CORRUPT;
        }
        return host_binding_is_empty(record) || host_binding_is_valid(record)
            ? DEVICE_IDENTITY_RESULT_OK
            : DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    if (record->state != DEVICE_IDENTITY_STATE_PENDING &&
        record->state != DEVICE_IDENTITY_STATE_ACTIVE) {
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    if (!device_id_is_valid(record->device_id) || !host_binding_is_valid(record)) {
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    uint8_t derived_public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES];
    if (device_ed25519_derive_public_key(record->private_seed, derived_public_key) !=
        DEVICE_CRYPTO_RESULT_OK) {
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    const bool keys_match =
        memcmp(derived_public_key, record->public_key, sizeof(derived_public_key)) == 0;
    memset(derived_public_key, 0, sizeof(derived_public_key));
    return keys_match ? DEVICE_IDENTITY_RESULT_OK : DEVICE_IDENTITY_RESULT_CORRUPT;
}

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
    output[2] = (uint8_t)((value >> 16U) & 0xFFU);
    output[3] = (uint8_t)(value >> 24U);
}

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

device_identity_result_t device_identity_record_encode(
    const device_identity_record_t *record,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (record == NULL || encoded == NULL || encoded_size == NULL ||
        encoded_capacity < DEVICE_IDENTITY_RECORD_ENCODED_BYTES) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    const device_identity_result_t validation = device_identity_record_validate(record);
    if (validation != DEVICE_IDENTITY_RESULT_OK) {
        return validation;
    }
    memset(encoded, 0, DEVICE_IDENTITY_RECORD_ENCODED_BYTES);
    write_u32_le(&encoded[0], RECORD_MAGIC);
    write_u16_le(&encoded[4], DEVICE_IDENTITY_RECORD_SCHEMA_VERSION);
    write_u16_le(&encoded[6], DEVICE_IDENTITY_RECORD_ENCODED_BYTES);
    encoded[8] = (uint8_t)record->state;
    memcpy(
        &encoded[RECORD_DEVICE_ID_OFFSET],
        record->device_id,
        strnlen(record->device_id, sizeof(record->device_id)));
    memcpy(
        &encoded[RECORD_INSTALLATION_ID_OFFSET],
        record->host_installation_id,
        strnlen(record->host_installation_id, sizeof(record->host_installation_id)));
    memcpy(
        &encoded[RECORD_FINGERPRINT_OFFSET],
        record->host_certificate_fingerprint,
        strnlen(
            record->host_certificate_fingerprint,
            sizeof(record->host_certificate_fingerprint)));
    memcpy(
        &encoded[RECORD_PRIVATE_SEED_OFFSET],
        record->private_seed,
        sizeof(record->private_seed));
    memcpy(
        &encoded[RECORD_PUBLIC_KEY_OFFSET],
        record->public_key,
        sizeof(record->public_key));
    *encoded_size = DEVICE_IDENTITY_RECORD_ENCODED_BYTES;
    return DEVICE_IDENTITY_RESULT_OK;
}

device_identity_result_t device_identity_record_decode(
    const uint8_t *encoded,
    size_t encoded_size,
    device_identity_record_t *record)
{
    if (encoded == NULL || record == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    if (encoded_size != DEVICE_IDENTITY_RECORD_ENCODED_BYTES ||
        read_u32_le(&encoded[0]) != RECORD_MAGIC ||
        read_u16_le(&encoded[4]) != DEVICE_IDENTITY_RECORD_SCHEMA_VERSION ||
        read_u16_le(&encoded[6]) != DEVICE_IDENTITY_RECORD_ENCODED_BYTES ||
        !bytes_are_zero(&encoded[9], RECORD_HEADER_BYTES - 9U)) {
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    device_identity_record_t decoded = {0};
    decoded.state = (device_identity_state_t)encoded[8];
    memcpy(
        decoded.device_id,
        &encoded[RECORD_DEVICE_ID_OFFSET],
        sizeof(decoded.device_id));
    memcpy(
        decoded.host_installation_id,
        &encoded[RECORD_INSTALLATION_ID_OFFSET],
        sizeof(decoded.host_installation_id));
    memcpy(
        decoded.host_certificate_fingerprint,
        &encoded[RECORD_FINGERPRINT_OFFSET],
        sizeof(decoded.host_certificate_fingerprint));
    memcpy(
        decoded.private_seed,
        &encoded[RECORD_PRIVATE_SEED_OFFSET],
        sizeof(decoded.private_seed));
    memcpy(
        decoded.public_key,
        &encoded[RECORD_PUBLIC_KEY_OFFSET],
        sizeof(decoded.public_key));
    if (memchr(decoded.device_id, '\0', sizeof(decoded.device_id)) == NULL ||
        memchr(
            decoded.host_installation_id,
            '\0',
            sizeof(decoded.host_installation_id)) == NULL ||
        memchr(
            decoded.host_certificate_fingerprint,
            '\0',
            sizeof(decoded.host_certificate_fingerprint)) == NULL) {
        memset(&decoded, 0, sizeof(decoded));
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    const device_identity_result_t validation = device_identity_record_validate(&decoded);
    if (validation != DEVICE_IDENTITY_RESULT_OK) {
        memset(&decoded, 0, sizeof(decoded));
        return validation;
    }
    *record = decoded;
    memset(&decoded, 0, sizeof(decoded));
    return DEVICE_IDENTITY_RESULT_OK;
}
