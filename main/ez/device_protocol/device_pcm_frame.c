#include "device_pcm_frame.h"

#include <stdbool.h>
#include <string.h>

#define DEVICE_PCM_PROTOCOL_VERSION 1U

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U)
           | ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

static bool direction_is_supported(device_pcm_direction_t direction)
{
    return direction == DEVICE_PCM_DIRECTION_UPLINK || direction == DEVICE_PCM_DIRECTION_DOWNLINK;
}

device_protocol_result_t device_pcm_frame_encode(
    device_pcm_direction_t direction,
    uint32_t stream_id,
    uint32_t sequence,
    const uint8_t *payload,
    size_t payload_size,
    uint8_t *frame,
    size_t frame_capacity)
{
    if (!direction_is_supported(direction) || stream_id == 0U || payload == NULL || frame == NULL
        || frame_capacity < DEVICE_PCM_FRAME_BYTES || payload_size == 0U
        || payload_size > DEVICE_PCM_PAYLOAD_BYTES || (payload_size % 2U) != 0U
        || (direction == DEVICE_PCM_DIRECTION_UPLINK && payload_size != DEVICE_PCM_PAYLOAD_BYTES)) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }

    memset(frame, 0, DEVICE_PCM_FRAME_BYTES);
    frame[0] = DEVICE_PCM_PROTOCOL_VERSION;
    frame[1] = (uint8_t)direction;
    write_u32_be(&frame[4], stream_id);
    write_u32_be(&frame[8], sequence);
    frame[12] = (uint8_t)(DEVICE_PCM_PAYLOAD_BYTES >> 8U);
    frame[13] = (uint8_t)DEVICE_PCM_PAYLOAD_BYTES;
    memcpy(&frame[DEVICE_PCM_HEADER_BYTES], payload, payload_size);
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_pcm_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    device_pcm_direction_t expected_direction,
    device_pcm_frame_view_t *frame_view)
{
    if (frame == NULL || frame_view == NULL || !direction_is_supported(expected_direction)) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (frame_size != DEVICE_PCM_FRAME_BYTES || frame[0] != DEVICE_PCM_PROTOCOL_VERSION
        || frame[1] != (uint8_t)expected_direction || frame[2] != 0U || frame[3] != 0U
        || read_u32_be(&frame[4]) == 0U
        || frame[12] != (uint8_t)(DEVICE_PCM_PAYLOAD_BYTES >> 8U)
        || frame[13] != (uint8_t)DEVICE_PCM_PAYLOAD_BYTES || frame[14] != 0U || frame[15] != 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_PCM_FRAME;
    }

    frame_view->direction = expected_direction;
    frame_view->stream_id = read_u32_be(&frame[4]);
    frame_view->sequence = read_u32_be(&frame[8]);
    frame_view->payload = &frame[DEVICE_PCM_HEADER_BYTES];
    frame_view->payload_size = DEVICE_PCM_PAYLOAD_BYTES;
    return DEVICE_PROTOCOL_RESULT_OK;
}
