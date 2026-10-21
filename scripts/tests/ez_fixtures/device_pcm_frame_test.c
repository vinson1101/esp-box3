#include "device_pcm_frame.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (uint8_t)(value - 'a' + 10);
    }
    assert(false && "fixture contains a non-hexadecimal character");
    return 0U;
}

static void decode_header_hex(const char *encoded, uint8_t header[DEVICE_PCM_HEADER_BYTES])
{
    assert(strlen(encoded) == DEVICE_PCM_HEADER_BYTES * 2U);
    for (size_t index = 0; index < DEVICE_PCM_HEADER_BYTES; ++index) {
        header[index] = (uint8_t)((hex_nibble(encoded[index * 2U]) << 4U)
                                  | hex_nibble(encoded[index * 2U + 1U]));
    }
}

static void verify_encoded_header(
    device_pcm_direction_t direction,
    const char *expected_header,
    uint32_t stream_id,
    uint32_t sequence)
{
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES] = {0};
    uint8_t frame[DEVICE_PCM_FRAME_BYTES] = {0};
    assert(device_pcm_frame_encode(
               direction,
               stream_id,
               sequence,
               payload,
               sizeof(payload),
               frame,
               sizeof(frame))
           == DEVICE_PROTOCOL_RESULT_OK);

    uint8_t expected[DEVICE_PCM_HEADER_BYTES] = {0};
    decode_header_hex(expected_header, expected);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);

    device_pcm_frame_view_t view = {0};
    assert(device_pcm_frame_decode(frame, sizeof(frame), direction, &view)
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(view.direction == direction);
    assert(view.stream_id == stream_id);
    assert(view.sequence == sequence);
    assert(view.payload_size == DEVICE_PCM_PAYLOAD_BYTES);
}

static void verify_invalid_header(const char *invalid_header)
{
    uint8_t frame[DEVICE_PCM_FRAME_BYTES] = {0};
    decode_header_hex(invalid_header, frame);
    device_pcm_frame_view_t view = {0};
    assert(device_pcm_frame_decode(
               frame,
               sizeof(frame),
               DEVICE_PCM_DIRECTION_UPLINK,
               &view)
           == DEVICE_PROTOCOL_RESULT_INVALID_PCM_FRAME);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count >= 6);
    const uint32_t stream_id = (uint32_t)strtoul(arguments[3], NULL, 10);
    const uint32_t sequence = (uint32_t)strtoul(arguments[4], NULL, 10);
    const size_t payload_size = (size_t)strtoul(arguments[5], NULL, 10);
    assert(payload_size == DEVICE_PCM_PAYLOAD_BYTES);

    verify_encoded_header(DEVICE_PCM_DIRECTION_UPLINK, arguments[1], stream_id, sequence);
    verify_encoded_header(DEVICE_PCM_DIRECTION_DOWNLINK, arguments[2], stream_id, sequence);
    for (int index = 6; index < argument_count; ++index) {
        verify_invalid_header(arguments[index]);
    }

    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES] = {0};
    uint8_t frame[DEVICE_PCM_FRAME_BYTES] = {0};
    assert(device_pcm_frame_encode(
               DEVICE_PCM_DIRECTION_UPLINK,
               0U,
               sequence,
               payload,
               sizeof(payload),
               frame,
               sizeof(frame))
           == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
    assert(device_pcm_frame_encode(
               DEVICE_PCM_DIRECTION_UPLINK,
               stream_id,
               sequence,
               payload,
               sizeof(payload) - 2U,
               frame,
               sizeof(frame))
           == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);

    assert(device_pcm_frame_encode(
               DEVICE_PCM_DIRECTION_DOWNLINK,
               stream_id,
               sequence,
               payload,
               sizeof(payload),
               frame,
               sizeof(frame))
           == DEVICE_PROTOCOL_RESULT_OK);
    memset(&frame[4], 0, sizeof(uint32_t));
    device_pcm_frame_view_t view = {0};
    assert(device_pcm_frame_decode(
               frame,
               sizeof(frame),
               DEVICE_PCM_DIRECTION_DOWNLINK,
               &view)
           == DEVICE_PROTOCOL_RESULT_INVALID_PCM_FRAME);

    puts("device PCM fixture and boundary checks passed");
    return 0;
}
