#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_crypto_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_ED25519_SEED_BYTES 32U
#define DEVICE_ED25519_PUBLIC_KEY_BYTES 32U
#define DEVICE_ED25519_SIGNATURE_BYTES 64U

device_crypto_result_t device_ed25519_derive_public_key(
    const uint8_t seed[DEVICE_ED25519_SEED_BYTES],
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES]);

device_crypto_result_t device_ed25519_sign(
    const uint8_t seed[DEVICE_ED25519_SEED_BYTES],
    const uint8_t *message,
    size_t message_size,
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES]);

device_crypto_result_t device_ed25519_verify(
    const uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t *message,
    size_t message_size,
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES]);

#ifdef __cplusplus
}
#endif
