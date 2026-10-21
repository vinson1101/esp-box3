#include "device_ed25519.h"

#include <stdbool.h>
#include <string.h>

#include "monocypher-ed25519.h"

#define MONOCYPHER_ED25519_SECRET_KEY_BYTES 64U

static bool message_is_valid(const uint8_t *message, size_t message_size)
{
    return message_size == 0U || message != NULL;
}

device_crypto_result_t device_ed25519_derive_public_key(
    const uint8_t seed[DEVICE_ED25519_SEED_BYTES],
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES])
{
    if (seed == NULL || public_key == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    uint8_t mutable_seed[DEVICE_ED25519_SEED_BYTES];
    uint8_t secret_key[MONOCYPHER_ED25519_SECRET_KEY_BYTES];
    memcpy(mutable_seed, seed, sizeof(mutable_seed));
    crypto_ed25519_key_pair(secret_key, public_key, mutable_seed);
    crypto_wipe(secret_key, sizeof(secret_key));
    return DEVICE_CRYPTO_RESULT_OK;
}

device_crypto_result_t device_ed25519_sign(
    const uint8_t seed[DEVICE_ED25519_SEED_BYTES],
    const uint8_t *message,
    size_t message_size,
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES])
{
    if (seed == NULL || signature == NULL || !message_is_valid(message, message_size)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    uint8_t mutable_seed[DEVICE_ED25519_SEED_BYTES];
    uint8_t secret_key[MONOCYPHER_ED25519_SECRET_KEY_BYTES];
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES];
    memcpy(mutable_seed, seed, sizeof(mutable_seed));
    crypto_ed25519_key_pair(secret_key, public_key, mutable_seed);
    crypto_ed25519_sign(signature, secret_key, message, message_size);
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(public_key, sizeof(public_key));
    return DEVICE_CRYPTO_RESULT_OK;
}

device_crypto_result_t device_ed25519_verify(
    const uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t *message,
    size_t message_size,
    const uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES])
{
    if (public_key == NULL || signature == NULL || !message_is_valid(message, message_size)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    return crypto_ed25519_check(signature, public_key, message, message_size) == 0
        ? DEVICE_CRYPTO_RESULT_OK
        : DEVICE_CRYPTO_RESULT_INVALID_SIGNATURE;
}
