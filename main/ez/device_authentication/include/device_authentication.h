#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_control_envelope.h"
#include "device_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_AUTHENTICATION_CONNECTION_ID_MAX_BYTES 256U
#define DEVICE_AUTHENTICATION_NONCE_BYTES 32U
#define DEVICE_AUTHENTICATION_NONCE_BASE64URL_BYTES 43U
#define DEVICE_AUTHENTICATION_NONCE_MAX_BYTES 256U
#define DEVICE_AUTHENTICATION_CLIENT_VERSION_MAX_BYTES 128U
#define DEVICE_AUTHENTICATION_ERROR_CODE_MAX_BYTES 64U

typedef enum {
    DEVICE_AUTHENTICATION_RESULT_OK = 0,
    DEVICE_AUTHENTICATION_RESULT_INVALID_ARGUMENT,
    DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE,
    DEVICE_AUTHENTICATION_RESULT_OUTPUT_TOO_SMALL,
    DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED,
} device_authentication_result_t;

typedef struct {
    char connection_id[DEVICE_AUTHENTICATION_CONNECTION_ID_MAX_BYTES + 1U];
    char host_nonce[DEVICE_AUTHENTICATION_NONCE_MAX_BYTES + 1U];
    int64_t server_time_ms;
} device_authentication_challenge_t;

typedef struct {
    device_capabilities_t capabilities;
    device_output_preference_t output_preference;
} device_authentication_ack_t;

typedef struct {
    char code[DEVICE_AUTHENTICATION_ERROR_CODE_MAX_BYTES + 1U];
    char correlation_message_id[DEVICE_MESSAGE_ID_MAX_BYTES + 1U];
    bool has_correlation_message_id;
    bool recoverable;
} device_authentication_error_t;

device_authentication_result_t device_authentication_decode_challenge(
    const device_control_envelope_t *envelope,
    device_authentication_challenge_t *challenge);

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
    size_t *encoded_size);

device_authentication_result_t device_authentication_decode_ack(
    const device_control_envelope_t *envelope,
    const char *expected_device_id,
    const char *expected_connection_id,
    device_authentication_ack_t *ack);

device_authentication_result_t device_authentication_decode_error(
    const device_control_envelope_t *envelope,
    device_authentication_error_t *error);

#ifdef __cplusplus
}
#endif
