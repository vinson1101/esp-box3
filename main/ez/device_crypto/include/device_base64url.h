#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_crypto_result.h"

#ifdef __cplusplus
extern "C" {
#endif

device_crypto_result_t device_base64url_encode(
    const uint8_t *input,
    size_t input_size,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_crypto_result_t device_base64url_decode(
    const char *encoded,
    size_t encoded_size,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size);

#ifdef __cplusplus
}
#endif
