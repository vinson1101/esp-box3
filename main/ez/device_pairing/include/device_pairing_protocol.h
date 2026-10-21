#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_control_envelope.h"
#include "device_ed25519.h"
#include "device_spake2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PAIRING_REQUEST_ID_MAX_BYTES 128U
#define DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES 128U
#define DEVICE_PAIRING_NONCE_MAX_BYTES 256U
#define DEVICE_PAIRING_DEVICE_ID_MAX_BYTES 256U

typedef enum {
    DEVICE_PAIRING_PROTOCOL_RESULT_OK = 0,
    DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT,
    DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE,
    DEVICE_PAIRING_PROTOCOL_RESULT_OUTPUT_TOO_SMALL,
    DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED,
} device_pairing_protocol_result_t;

typedef struct {
    int64_t expires_at_ms;
} device_pairing_pending_t;

typedef struct {
    char host_nonce[DEVICE_PAIRING_NONCE_MAX_BYTES + 1U];
    uint8_t host_share[DEVICE_SPAKE2_SHARE_BYTES];
    uint8_t host_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES];
} device_pairing_pake_t;

typedef struct {
    char device_id[DEVICE_PAIRING_DEVICE_ID_MAX_BYTES + 1U];
    uint8_t host_proof[DEVICE_SPAKE2_BINDING_MAC_BYTES];
} device_pairing_bind_ack_t;

typedef struct {
    char display_name[DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES + 1U];
} device_pairing_complete_t;

device_pairing_protocol_result_t device_pairing_encode_hello(
    const char *message_id,
    const char *pairing_request_id,
    const char *display_name,
    const char *device_nonce,
    const device_capabilities_t *capabilities,
    const uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_pairing_protocol_result_t device_pairing_decode_pending(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_pending_t *pending);

device_pairing_protocol_result_t device_pairing_decode_pake(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_pake_t *pake);

device_pairing_protocol_result_t device_pairing_encode_confirmation(
    const char *message_id,
    const char *pairing_request_id,
    const uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_pairing_protocol_result_t device_pairing_encode_bind(
    const char *message_id,
    const char *pairing_request_id,
    const uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES],
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_pairing_protocol_result_t device_pairing_decode_bind_ack(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    device_pairing_bind_ack_t *ack);

device_pairing_protocol_result_t device_pairing_encode_commit(
    const char *message_id,
    const char *pairing_request_id,
    const char *device_id,
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES],
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES],
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_pairing_protocol_result_t device_pairing_decode_complete(
    const device_control_envelope_t *envelope,
    const char *expected_pairing_request_id,
    const char *expected_device_id,
    device_pairing_complete_t *complete);

#ifdef __cplusplus
}
#endif
