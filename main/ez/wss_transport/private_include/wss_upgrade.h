#ifndef WSS_UPGRADE_H
#define WSS_UPGRADE_H

#include <stddef.h>
#include <stdint.h>

#include "wss_frame_codec.h"

#define WSS_CLIENT_KEY_BYTES 16U
#define WSS_CLIENT_KEY_TEXT_CAPACITY 25U
#define WSS_ACCEPT_TEXT_CAPACITY 29U
#define WSS_UPGRADE_HEADER_MAX_BYTES 4096U

wss_codec_result_t wss_client_key_encode(
    const uint8_t key[WSS_CLIENT_KEY_BYTES],
    char output[WSS_CLIENT_KEY_TEXT_CAPACITY]);

wss_codec_result_t wss_upgrade_expected_accept(
    const char *client_key,
    char output[WSS_ACCEPT_TEXT_CAPACITY]);

wss_codec_result_t wss_upgrade_build_request(
    const char *host,
    uint16_t port,
    const char *path,
    const char *client_key,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

wss_codec_result_t wss_upgrade_find_header_end(
    const uint8_t *input,
    size_t input_length,
    size_t *header_length);

wss_codec_result_t wss_upgrade_validate_response(
    const uint8_t *response,
    size_t response_length,
    const char *expected_accept);

#endif
