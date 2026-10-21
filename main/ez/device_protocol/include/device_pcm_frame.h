#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PCM_HEADER_BYTES 16U
#define DEVICE_PCM_PAYLOAD_BYTES 640U
#define DEVICE_PCM_FRAME_BYTES (DEVICE_PCM_HEADER_BYTES + DEVICE_PCM_PAYLOAD_BYTES)

typedef enum {
    DEVICE_PCM_DIRECTION_UPLINK = 1,
    DEVICE_PCM_DIRECTION_DOWNLINK = 2,
} device_pcm_direction_t;

typedef struct {
    device_pcm_direction_t direction;
    uint32_t stream_id;
    uint32_t sequence;
    const uint8_t *payload;
    size_t payload_size;
} device_pcm_frame_view_t;

device_protocol_result_t device_pcm_frame_encode(
    device_pcm_direction_t direction,
    uint32_t stream_id,
    uint32_t sequence,
    const uint8_t *payload,
    size_t payload_size,
    uint8_t *frame,
    size_t frame_capacity);

device_protocol_result_t device_pcm_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    device_pcm_direction_t expected_direction,
    device_pcm_frame_view_t *frame_view);

#ifdef __cplusplus
}
#endif
