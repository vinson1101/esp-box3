#include "device_base64url.h"

#include <stdint.h>

static const char BASE64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int base64url_value(char encoded)
{
    if (encoded >= 'A' && encoded <= 'Z') {
        return encoded - 'A';
    }
    if (encoded >= 'a' && encoded <= 'z') {
        return encoded - 'a' + 26;
    }
    if (encoded >= '0' && encoded <= '9') {
        return encoded - '0' + 52;
    }
    if (encoded == '-') {
        return 62;
    }
    if (encoded == '_') {
        return 63;
    }
    return -1;
}

device_crypto_result_t device_base64url_encode(
    const uint8_t *input,
    size_t input_size,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (encoded_size == NULL || (input_size > 0U && input == NULL)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    if (input_size / 3U > SIZE_MAX / 4U) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    const size_t required_size = (input_size / 3U) * 4U
                                 + (input_size % 3U == 0U ? 0U : input_size % 3U + 1U);
    *encoded_size = required_size;
    if (required_size > encoded_capacity || (required_size > 0U && encoded == NULL)) {
        return DEVICE_CRYPTO_RESULT_OUTPUT_TOO_SMALL;
    }

    size_t input_offset = 0U;
    size_t encoded_offset = 0U;
    while (input_size - input_offset >= 3U) {
        const uint32_t block = ((uint32_t)input[input_offset] << 16U)
                               | ((uint32_t)input[input_offset + 1U] << 8U)
                               | input[input_offset + 2U];
        encoded[encoded_offset] = BASE64URL_ALPHABET[(block >> 18U) & 0x3fU];
        encoded[encoded_offset + 1U] = BASE64URL_ALPHABET[(block >> 12U) & 0x3fU];
        encoded[encoded_offset + 2U] = BASE64URL_ALPHABET[(block >> 6U) & 0x3fU];
        encoded[encoded_offset + 3U] = BASE64URL_ALPHABET[block & 0x3fU];
        input_offset += 3U;
        encoded_offset += 4U;
    }

    const size_t remaining = input_size - input_offset;
    if (remaining == 1U) {
        const uint32_t block = (uint32_t)input[input_offset] << 16U;
        encoded[encoded_offset] = BASE64URL_ALPHABET[(block >> 18U) & 0x3fU];
        encoded[encoded_offset + 1U] = BASE64URL_ALPHABET[(block >> 12U) & 0x3fU];
    } else if (remaining == 2U) {
        const uint32_t block = ((uint32_t)input[input_offset] << 16U)
                               | ((uint32_t)input[input_offset + 1U] << 8U);
        encoded[encoded_offset] = BASE64URL_ALPHABET[(block >> 18U) & 0x3fU];
        encoded[encoded_offset + 1U] = BASE64URL_ALPHABET[(block >> 12U) & 0x3fU];
        encoded[encoded_offset + 2U] = BASE64URL_ALPHABET[(block >> 6U) & 0x3fU];
    }
    return DEVICE_CRYPTO_RESULT_OK;
}

device_crypto_result_t device_base64url_decode(
    const char *encoded,
    size_t encoded_size,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size)
{
    if (output_size == NULL || (encoded_size > 0U && encoded == NULL)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    if (encoded_size % 4U == 1U) {
        return DEVICE_CRYPTO_RESULT_INVALID_ENCODING;
    }
    if (encoded_size > SIZE_MAX / 6U) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    const size_t required_size = (encoded_size * 6U) / 8U;
    *output_size = required_size;
    if (required_size > output_capacity || (required_size > 0U && output == NULL)) {
        return DEVICE_CRYPTO_RESULT_OUTPUT_TOO_SMALL;
    }

    uint32_t accumulator = 0U;
    unsigned int accumulator_bits = 0U;
    size_t output_offset = 0U;
    for (size_t index = 0U; index < encoded_size; ++index) {
        const int value = base64url_value(encoded[index]);
        if (value < 0) {
            return DEVICE_CRYPTO_RESULT_INVALID_ENCODING;
        }
        accumulator = (accumulator << 6U) | (uint32_t)value;
        accumulator_bits += 6U;
        if (accumulator_bits >= 8U) {
            accumulator_bits -= 8U;
            output[output_offset++] = (uint8_t)(accumulator >> accumulator_bits);
            if (accumulator_bits == 0U) {
                accumulator = 0U;
            } else {
                accumulator &= (1U << accumulator_bits) - 1U;
            }
        }
    }
    if (accumulator != 0U || output_offset != required_size) {
        return DEVICE_CRYPTO_RESULT_INVALID_ENCODING;
    }
    return DEVICE_CRYPTO_RESULT_OK;
}
