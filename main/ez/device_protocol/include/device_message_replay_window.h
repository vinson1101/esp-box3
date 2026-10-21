#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_protocol_limits.h"
#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY 256U

typedef struct {
    char message_ids[DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY][DEVICE_MESSAGE_ID_MAX_BYTES + 1U];
    uint16_t oldest_index;
    uint16_t message_count;
} device_message_replay_window_t;

void device_message_replay_window_initialize(device_message_replay_window_t *window);

device_protocol_result_t device_message_replay_window_accept(
    device_message_replay_window_t *window,
    const char *message_id);

#ifdef __cplusplus
}
#endif
