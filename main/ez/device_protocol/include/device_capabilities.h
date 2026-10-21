#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CAPABILITIES_BYTES 7U

typedef struct {
    bool input_text;
    bool input_pcm16_16k_mono;
    bool output_text;
    bool output_pcm16_16k_mono;
    bool playback_cancel;
    bool display_status;
    bool display_transcript;
} device_capabilities_t;

typedef enum {
    DEVICE_OUTPUT_PREFERENCE_TEXT = 0,
    DEVICE_OUTPUT_PREFERENCE_AUDIO,
    DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO,
} device_output_preference_t;

device_protocol_result_t device_capabilities_serialize(
    const device_capabilities_t *capabilities,
    uint8_t encoded[DEVICE_CAPABILITIES_BYTES]);

#ifdef __cplusplus
}
#endif
