#include "wss_upgrade.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"

#define WSS_UPGRADE_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static const char BASE64_DIGITS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static wss_codec_result_t base64_encode(
    const uint8_t *input,
    size_t input_length,
    char *output,
    size_t output_capacity)
{
    const size_t encoded_length = ((input_length + 2U) / 3U) * 4U;
    if ((input == NULL && input_length != 0U) || output == NULL ||
        output_capacity <= encoded_length) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    size_t input_cursor = 0U;
    size_t output_cursor = 0U;
    while (input_cursor < input_length) {
        const size_t remaining = input_length - input_cursor;
        const uint32_t first = input[input_cursor++];
        const uint32_t second = remaining > 1U ? input[input_cursor++] : 0U;
        const uint32_t third = remaining > 2U ? input[input_cursor++] : 0U;
        const uint32_t group = (first << 16U) | (second << 8U) | third;
        output[output_cursor++] = BASE64_DIGITS[(group >> 18U) & 0x3fU];
        output[output_cursor++] = BASE64_DIGITS[(group >> 12U) & 0x3fU];
        output[output_cursor++] = remaining > 1U ? BASE64_DIGITS[(group >> 6U) & 0x3fU] : '=';
        output[output_cursor++] = remaining > 2U ? BASE64_DIGITS[group & 0x3fU] : '=';
    }
    output[output_cursor] = '\0';
    return WSS_CODEC_RESULT_OK;
}

static bool ascii_equal_case_insensitive(const uint8_t *value, size_t length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    if (length != expected_length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (tolower((unsigned char)value[index]) != tolower((unsigned char)expected[index])) {
            return false;
        }
    }
    return true;
}

static bool ascii_equal(const uint8_t *value, size_t length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    return length == expected_length && memcmp(value, expected, length) == 0;
}

static bool header_token_contains(
    const uint8_t *value,
    size_t length,
    const char *expected_token)
{
    size_t cursor = 0U;
    while (cursor < length) {
        while (cursor < length && (value[cursor] == ' ' || value[cursor] == '\t' ||
                                   value[cursor] == ',')) {
            ++cursor;
        }
        const size_t token_start = cursor;
        while (cursor < length && value[cursor] != ',') {
            ++cursor;
        }
        size_t token_end = cursor;
        while (token_end > token_start &&
               (value[token_end - 1U] == ' ' || value[token_end - 1U] == '\t')) {
            --token_end;
        }
        if (ascii_equal_case_insensitive(
                &value[token_start], token_end - token_start, expected_token)) {
            return true;
        }
    }
    return false;
}

wss_codec_result_t wss_client_key_encode(
    const uint8_t key[WSS_CLIENT_KEY_BYTES],
    char output[WSS_CLIENT_KEY_TEXT_CAPACITY])
{
    return base64_encode(key, WSS_CLIENT_KEY_BYTES, output, WSS_CLIENT_KEY_TEXT_CAPACITY);
}

wss_codec_result_t wss_upgrade_expected_accept(
    const char *client_key,
    char output[WSS_ACCEPT_TEXT_CAPACITY])
{
    if (client_key == NULL || output == NULL || strlen(client_key) != 24U) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    uint8_t input[24U + sizeof(WSS_UPGRADE_GUID) - 1U];
    memcpy(input, client_key, 24U);
    memcpy(&input[24U], WSS_UPGRADE_GUID, sizeof(WSS_UPGRADE_GUID) - 1U);

    const mbedtls_md_info_t *sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    uint8_t digest[20U];
    if (sha1 == NULL || mbedtls_md(sha1, input, sizeof(input), digest) != 0) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }
    return base64_encode(digest, sizeof(digest), output, WSS_ACCEPT_TEXT_CAPACITY);
}

wss_codec_result_t wss_upgrade_build_request(
    const char *host,
    uint16_t port,
    const char *path,
    const char *client_key,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (host == NULL || host[0] == '\0' || port == 0U || path == NULL || path[0] != '/' ||
        client_key == NULL || strlen(client_key) != 24U || output == NULL ||
        output_length == NULL) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    const int written = snprintf(
        (char *)output,
        output_capacity,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path,
        host,
        (unsigned int)port,
        client_key);
    if (written < 0 || (size_t)written >= output_capacity) {
        return WSS_CODEC_RESULT_CAPACITY_EXCEEDED;
    }
    *output_length = (size_t)written;
    return WSS_CODEC_RESULT_OK;
}

wss_codec_result_t wss_upgrade_find_header_end(
    const uint8_t *input,
    size_t input_length,
    size_t *header_length)
{
    if (input == NULL || header_length == NULL) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    for (size_t index = 3U; index < input_length; ++index) {
        if (input[index - 3U] == '\r' && input[index - 2U] == '\n' &&
            input[index - 1U] == '\r' && input[index] == '\n') {
            *header_length = index + 1U;
            return WSS_CODEC_RESULT_OK;
        }
    }
    return input_length >= WSS_UPGRADE_HEADER_MAX_BYTES ? WSS_CODEC_RESULT_CAPACITY_EXCEEDED
                                                        : WSS_CODEC_RESULT_INCOMPLETE;
}

wss_codec_result_t wss_upgrade_validate_response(
    const uint8_t *response,
    size_t response_length,
    const char *expected_accept)
{
    if (response == NULL || expected_accept == NULL) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    size_t header_length = 0U;
    if (wss_upgrade_find_header_end(response, response_length, &header_length) !=
        WSS_CODEC_RESULT_OK) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }

    const uint8_t *line_end = memchr(response, '\n', header_length);
    if (line_end == NULL) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }
    const size_t status_length = (size_t)(line_end - response);
    if (status_length < 12U || memcmp(response, "HTTP/1.1 101", 12U) != 0 ||
        (status_length > 12U && response[12U] != ' ' && response[12U] != '\r')) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }

    bool upgrade_valid = false;
    bool connection_valid = false;
    bool accept_valid = false;
    bool upgrade_seen = false;
    bool connection_seen = false;
    bool accept_seen = false;
    size_t cursor = status_length + 1U;
    while (cursor + 2U < header_length) {
        const size_t line_start = cursor;
        while (cursor < header_length && response[cursor] != '\n') {
            ++cursor;
        }
        size_t content_end = cursor;
        if (content_end > line_start && response[content_end - 1U] == '\r') {
            --content_end;
        }
        ++cursor;
        if (content_end == line_start) {
            break;
        }
        const uint8_t *colon = memchr(&response[line_start], ':', content_end - line_start);
        if (colon == NULL) {
            return WSS_CODEC_RESULT_PROTOCOL_ERROR;
        }
        const size_t name_length = (size_t)(colon - &response[line_start]);
        size_t value_start = (size_t)(colon - response) + 1U;
        while (value_start < content_end &&
               (response[value_start] == ' ' || response[value_start] == '\t')) {
            ++value_start;
        }
        while (content_end > value_start &&
               (response[content_end - 1U] == ' ' || response[content_end - 1U] == '\t')) {
            --content_end;
        }
        if (ascii_equal_case_insensitive(&response[line_start], name_length, "Upgrade")) {
            if (upgrade_seen) {
                return WSS_CODEC_RESULT_PROTOCOL_ERROR;
            }
            upgrade_seen = true;
            upgrade_valid = header_token_contains(
                &response[value_start], content_end - value_start, "websocket");
        } else if (ascii_equal_case_insensitive(
                       &response[line_start], name_length, "Connection")) {
            if (connection_seen) {
                return WSS_CODEC_RESULT_PROTOCOL_ERROR;
            }
            connection_seen = true;
            connection_valid = header_token_contains(
                &response[value_start], content_end - value_start, "Upgrade");
        } else if (ascii_equal_case_insensitive(
                       &response[line_start], name_length, "Sec-WebSocket-Accept")) {
            if (accept_seen) {
                return WSS_CODEC_RESULT_PROTOCOL_ERROR;
            }
            accept_seen = true;
            accept_valid = ascii_equal(
                &response[value_start], content_end - value_start, expected_accept);
        }
    }
    return upgrade_valid && connection_valid && accept_valid ? WSS_CODEC_RESULT_OK
                                                             : WSS_CODEC_RESULT_PROTOCOL_ERROR;
}
