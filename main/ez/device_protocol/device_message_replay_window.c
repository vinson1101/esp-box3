#include "device_message_replay_window.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool message_id_is_valid(const char *message_id, size_t *message_id_size)
{
    if (message_id == NULL || message_id_size == NULL) {
        return false;
    }
    const size_t size = strlen(message_id);
    if (size == 0U || size > DEVICE_MESSAGE_ID_MAX_BYTES) {
        return false;
    }
    bool visible_content = false;
    for (size_t index = 0U; index < size; ++index) {
        if (!isspace((unsigned char)message_id[index])) {
            visible_content = true;
            break;
        }
    }
    if (!visible_content) {
        return false;
    }
    *message_id_size = size;
    return true;
}

void device_message_replay_window_initialize(device_message_replay_window_t *window)
{
    if (window != NULL) {
        memset(window, 0, sizeof(*window));
    }
}

device_protocol_result_t device_message_replay_window_accept(
    device_message_replay_window_t *window,
    const char *message_id)
{
    size_t message_id_size = 0U;
    if (window == NULL || !message_id_is_valid(message_id, &message_id_size)
        || window->oldest_index >= DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY
        || window->message_count > DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }

    for (uint16_t offset = 0U; offset < window->message_count; ++offset) {
        const uint16_t index =
            (uint16_t)((window->oldest_index + offset) % DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY);
        if (strcmp(window->message_ids[index], message_id) == 0) {
            return DEVICE_PROTOCOL_RESULT_DUPLICATE_MESSAGE_ID;
        }
    }

    uint16_t insertion_index = 0U;
    if (window->message_count < DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY) {
        insertion_index = (uint16_t)(
            (window->oldest_index + window->message_count)
            % DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY);
        window->message_count += 1U;
    } else {
        insertion_index = window->oldest_index;
        window->oldest_index =
            (uint16_t)((window->oldest_index + 1U) % DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY);
    }

    memcpy(window->message_ids[insertion_index], message_id, message_id_size + 1U);
    return DEVICE_PROTOCOL_RESULT_OK;
}
