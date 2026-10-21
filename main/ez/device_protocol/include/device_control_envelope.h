#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "device_protocol_limits.h"
#include "device_protocol_result.h"
#include "device_protocol_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct device_control_envelope device_control_envelope_t;

device_protocol_result_t device_control_envelope_decode(
    const char *encoded,
    size_t encoded_size,
    device_control_envelope_t **envelope);

device_protocol_result_t device_control_envelope_encode(
    const char *message_id,
    const char *message_type,
    const cJSON *payload,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

uint16_t device_control_envelope_protocol_minor(const device_control_envelope_t *envelope);
const char *device_control_envelope_message_id(const device_control_envelope_t *envelope);
const char *device_control_envelope_message_type(const device_control_envelope_t *envelope);
const cJSON *device_control_envelope_payload(const device_control_envelope_t *envelope);
void device_control_envelope_release(device_control_envelope_t *envelope);

#ifdef __cplusplus
}
#endif
