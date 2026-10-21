#include "wss_frame_codec.h"

#include <string.h>

static bool opcode_is_valid(wss_opcode_t opcode)
{
    return opcode == WSS_OPCODE_CONTINUATION || opcode == WSS_OPCODE_TEXT ||
           opcode == WSS_OPCODE_BINARY || opcode == WSS_OPCODE_CLOSE ||
           opcode == WSS_OPCODE_PING || opcode == WSS_OPCODE_PONG;
}

static bool opcode_is_control(wss_opcode_t opcode)
{
    return (uint8_t)opcode >= 0x08U;
}

wss_codec_result_t wss_frame_encode_client(
    wss_opcode_t opcode,
    bool final,
    const uint8_t mask[4],
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (!opcode_is_valid(opcode) || mask == NULL || (payload == NULL && payload_length != 0U) ||
        output == NULL || output_length == NULL) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    if (opcode_is_control(opcode) && (!final || payload_length > WSS_CONTROL_PAYLOAD_MAX_BYTES)) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }

    size_t length_bytes = 0U;
    uint8_t length_marker = 0U;
    if (payload_length <= 125U) {
        length_marker = (uint8_t)payload_length;
    } else if (payload_length <= UINT16_MAX) {
        length_marker = 126U;
        length_bytes = 2U;
    } else {
        length_marker = 127U;
        length_bytes = 8U;
    }
    const size_t header_length = 2U + length_bytes + 4U;
    if (payload_length > SIZE_MAX - header_length || output_capacity < header_length + payload_length) {
        return WSS_CODEC_RESULT_CAPACITY_EXCEEDED;
    }

    output[0] = (final ? 0x80U : 0U) | (uint8_t)opcode;
    output[1] = 0x80U | length_marker;
    size_t cursor = 2U;
    if (length_bytes == 2U) {
        output[cursor++] = (uint8_t)(payload_length >> 8U);
        output[cursor++] = (uint8_t)payload_length;
    } else if (length_bytes == 8U) {
        const uint64_t length = payload_length;
        for (size_t index = 0U; index < 8U; ++index) {
            output[cursor++] = (uint8_t)(length >> (56U - (index * 8U)));
        }
    }
    memcpy(&output[cursor], mask, 4U);
    cursor += 4U;
    for (size_t index = 0U; index < payload_length; ++index) {
        output[cursor + index] = payload[index] ^ mask[index % 4U];
    }
    *output_length = cursor + payload_length;
    return WSS_CODEC_RESULT_OK;
}

wss_codec_result_t wss_frame_decode_server_header(
    const uint8_t *input,
    size_t input_length,
    wss_frame_header_t *header)
{
    if (input == NULL || header == NULL) {
        return WSS_CODEC_RESULT_INVALID_ARGUMENT;
    }
    if (input_length < 2U) {
        return WSS_CODEC_RESULT_INCOMPLETE;
    }
    if ((input[0] & 0x70U) != 0U || (input[1] & 0x80U) != 0U) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }

    const wss_opcode_t opcode = (wss_opcode_t)(input[0] & 0x0fU);
    if (!opcode_is_valid(opcode)) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }
    const bool final = (input[0] & 0x80U) != 0U;
    const uint8_t marker = input[1] & 0x7fU;
    uint64_t payload_length = marker;
    size_t header_length = 2U;
    if (marker == 126U) {
        if (input_length < 4U) {
            return WSS_CODEC_RESULT_INCOMPLETE;
        }
        payload_length = ((uint64_t)input[2] << 8U) | input[3];
        header_length = 4U;
        if (payload_length < 126U) {
            return WSS_CODEC_RESULT_PROTOCOL_ERROR;
        }
    } else if (marker == 127U) {
        if (input_length < 10U) {
            return WSS_CODEC_RESULT_INCOMPLETE;
        }
        if ((input[2] & 0x80U) != 0U) {
            return WSS_CODEC_RESULT_PROTOCOL_ERROR;
        }
        payload_length = 0U;
        for (size_t index = 0U; index < 8U; ++index) {
            payload_length = (payload_length << 8U) | input[2U + index];
        }
        header_length = 10U;
        if (payload_length <= UINT16_MAX) {
            return WSS_CODEC_RESULT_PROTOCOL_ERROR;
        }
    }

    if (opcode_is_control(opcode) && (!final || payload_length > WSS_CONTROL_PAYLOAD_MAX_BYTES)) {
        return WSS_CODEC_RESULT_PROTOCOL_ERROR;
    }
    header->final = final;
    header->opcode = opcode;
    header->payload_length = payload_length;
    header->header_length = header_length;
    return WSS_CODEC_RESULT_OK;
}
