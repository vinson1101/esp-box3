#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_crypto_result.h"

#ifdef __cplusplus
extern "C" {
#endif

device_crypto_result_t device_pairing_associated_data_encode(
    const char *pairing_request_id,
    const char *installation_id,
    const char *certificate_fingerprint,
    const char *device_nonce,
    const char *host_nonce,
    const device_capabilities_t *capabilities,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_crypto_result_t device_pairing_bind_transcript_encode(
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t public_key[32],
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_crypto_result_t device_pairing_commit_transcript_encode(
    const uint8_t *bind_transcript,
    size_t bind_transcript_size,
    const char *device_id,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_crypto_result_t device_authentication_transcript_encode(
    const char *connection_id,
    const char *host_nonce,
    const char *device_id,
    const char *device_nonce,
    const device_capabilities_t *capabilities,
    device_output_preference_t output_preference,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_crypto_result_t device_pairing_password_transcript_encode(
    const char *pairing_code,
    const char *pairing_request_id,
    const char *installation_id,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

#ifdef __cplusplus
}
#endif
