#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_control_envelope.h"
#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_APPLICATION_HEARTBEAT_NONCE_MAX_BYTES 256U

typedef struct {
    char nonce[DEVICE_APPLICATION_HEARTBEAT_NONCE_MAX_BYTES + 1U];
    int64_t sent_at_ms;
} device_application_heartbeat_t;

device_protocol_result_t
device_application_heartbeat_decode_ping(const device_control_envelope_t *envelope,
                                         device_application_heartbeat_t *heartbeat);

device_protocol_result_t device_application_heartbeat_encode_pong(
    const char *message_id, const device_application_heartbeat_t *heartbeat, char *encoded,
    size_t encoded_capacity, size_t *encoded_size);

#ifdef __cplusplus
}
#endif
