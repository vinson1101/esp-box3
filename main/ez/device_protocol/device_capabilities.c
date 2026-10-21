#include "device_capabilities.h"

#include <stddef.h>

device_protocol_result_t device_capabilities_serialize(
    const device_capabilities_t *capabilities,
    uint8_t encoded[DEVICE_CAPABILITIES_BYTES])
{
    if (capabilities == NULL || encoded == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    encoded[0] = capabilities->input_text ? 1U : 0U;
    encoded[1] = capabilities->input_pcm16_16k_mono ? 1U : 0U;
    encoded[2] = capabilities->output_text ? 1U : 0U;
    encoded[3] = capabilities->output_pcm16_16k_mono ? 1U : 0U;
    encoded[4] = capabilities->playback_cancel ? 1U : 0U;
    encoded[5] = capabilities->display_status ? 1U : 0U;
    encoded[6] = capabilities->display_transcript ? 1U : 0U;
    return DEVICE_PROTOCOL_RESULT_OK;
}
