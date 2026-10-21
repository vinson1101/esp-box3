#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_crypto_result.h"
#include "device_random_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_SPAKE2_SHARE_BYTES 65U
#define DEVICE_SPAKE2_CONFIRMATION_BYTES 32U
#define DEVICE_SPAKE2_BINDING_MAC_BYTES 32U
#define DEVICE_SPAKE2_IDENTITY_MAX_BYTES 256U
#define DEVICE_SPAKE2_ASSOCIATED_DATA_MAX_BYTES 2048U

typedef struct device_spake2_party_a device_spake2_party_a_t;
typedef struct device_spake2_keys device_spake2_keys_t;

/* The random source and its context must remain valid until the session is released. */
device_crypto_result_t device_spake2_party_a_begin(
    const char *pairing_code,
    const char *pairing_request_id,
    const char *installation_id,
    const device_random_source_t *random_source,
    device_spake2_party_a_t **session,
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES]);

/* A finish attempt consumes the session secrets and makes the session single-use. */
device_crypto_result_t device_spake2_party_a_finish(
    device_spake2_party_a_t *session,
    const uint8_t host_share[DEVICE_SPAKE2_SHARE_BYTES],
    const uint8_t *associated_data,
    size_t associated_data_size,
    device_spake2_keys_t **keys);

void device_spake2_party_a_release(device_spake2_party_a_t *session);

device_crypto_result_t device_spake2_keys_copy_device_confirmation(
    const device_spake2_keys_t *keys,
    uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES]);

device_crypto_result_t device_spake2_keys_verify_host_confirmation(
    const device_spake2_keys_t *keys,
    const uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES]);

device_crypto_result_t device_spake2_keys_binding_mac(
    const device_spake2_keys_t *keys,
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES]);

device_crypto_result_t device_spake2_keys_verify_binding_mac(
    const device_spake2_keys_t *keys,
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES]);

void device_spake2_keys_release(device_spake2_keys_t *keys);

#ifdef __cplusplus
}
#endif
