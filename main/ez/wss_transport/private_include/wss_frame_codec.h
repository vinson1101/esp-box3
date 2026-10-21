#ifndef WSS_FRAME_CODEC_H
#define WSS_FRAME_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WSS_FRAME_HEADER_MAX_BYTES 14U
#define WSS_CONTROL_PAYLOAD_MAX_BYTES 125U

typedef enum {
    WSS_CODEC_RESULT_OK = 0,
    WSS_CODEC_RESULT_INCOMPLETE,
    WSS_CODEC_RESULT_INVALID_ARGUMENT,
    WSS_CODEC_RESULT_PROTOCOL_ERROR,
    WSS_CODEC_RESULT_CAPACITY_EXCEEDED,
} wss_codec_result_t;

typedef enum {
    WSS_OPCODE_CONTINUATION = 0x0,
    WSS_OPCODE_TEXT = 0x1,
    WSS_OPCODE_BINARY = 0x2,
    WSS_OPCODE_CLOSE = 0x8,
    WSS_OPCODE_PING = 0x9,
    WSS_OPCODE_PONG = 0xA,
} wss_opcode_t;

typedef struct {
    bool final;
    wss_opcode_t opcode;
    uint64_t payload_length;
    size_t header_length;
} wss_frame_header_t;

wss_codec_result_t wss_frame_encode_client(
    wss_opcode_t opcode,
    bool final,
    const uint8_t mask[4],
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

wss_codec_result_t wss_frame_decode_server_header(
    const uint8_t *input,
    size_t input_length,
    wss_frame_header_t *header);

#endif
