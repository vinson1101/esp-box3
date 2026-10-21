#ifndef WSS_TRANSPORT_H
#define WSS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_protocol_limits.h"

#define WSS_TRANSPORT_PATH_CAPACITY 16U
#define WSS_TRANSPORT_FINGERPRINT_LENGTH 64U
#define WSS_TRANSPORT_CONTROL_MAX_BYTES DEVICE_CONTROL_MESSAGE_MAX_BYTES

typedef struct wss_transport wss_transport_t;

typedef bool (*wss_transport_cancel_callback_t)(void *context);

typedef enum {
    WSS_TRANSPORT_RESULT_OK = 0,
    WSS_TRANSPORT_RESULT_INVALID_ARGUMENT,
    WSS_TRANSPORT_RESULT_NO_MEMORY,
    WSS_TRANSPORT_RESULT_TIMEOUT,
    WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE,
    WSS_TRANSPORT_RESULT_TLS_FAILED,
    WSS_TRANSPORT_RESULT_CERTIFICATE_UNAVAILABLE,
    WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH,
    WSS_TRANSPORT_RESULT_UPGRADE_REJECTED,
    WSS_TRANSPORT_RESULT_PROTOCOL_ERROR,
    WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED,
    WSS_TRANSPORT_RESULT_CONNECTION_CLOSED,
    WSS_TRANSPORT_RESULT_CANCELLED,
} wss_transport_result_t;

typedef enum {
    WSS_TRANSPORT_MESSAGE_TEXT = 0,
    WSS_TRANSPORT_MESSAGE_BINARY,
    WSS_TRANSPORT_MESSAGE_PING,
    WSS_TRANSPORT_MESSAGE_PONG,
    WSS_TRANSPORT_MESSAGE_CLOSE,
} wss_transport_message_type_t;

typedef struct {
    uint32_t ipv4_address;
    uint16_t port;
    char path[WSS_TRANSPORT_PATH_CAPACITY];
    char certificate_fingerprint[WSS_TRANSPORT_FINGERPRINT_LENGTH + 1U];
} wss_transport_endpoint_t;

typedef struct {
    wss_transport_message_type_t type;
    size_t payload_length;
    uint16_t close_code;
} wss_transport_message_t;

wss_transport_result_t wss_transport_create(wss_transport_t **transport);

/*
 * The callback is polled from blocking transport operations and must not
 * block. Returning true cancels the current operation and closes its socket.
 */
void wss_transport_set_cancel_callback(
    wss_transport_t *transport,
    wss_transport_cancel_callback_t callback,
    void *context);

void wss_transport_destroy(wss_transport_t *transport);

wss_transport_result_t wss_transport_connect(
    wss_transport_t *transport,
    const wss_transport_endpoint_t *endpoint,
    uint32_t tcp_timeout_ms,
    uint32_t tls_timeout_ms,
    uint32_t upgrade_timeout_ms);

void wss_transport_disconnect(wss_transport_t *transport);

bool wss_transport_is_connected(const wss_transport_t *transport);

/* Waits for encrypted or socket input without consuming a WebSocket frame. */
wss_transport_result_t wss_transport_wait_readable(
    wss_transport_t *transport,
    uint32_t timeout_ms,
    bool *readable);

wss_transport_result_t wss_transport_send_text(
    wss_transport_t *transport,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t timeout_ms);

wss_transport_result_t wss_transport_send_binary(
    wss_transport_t *transport,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t timeout_ms);

wss_transport_result_t wss_transport_send_close(
    wss_transport_t *transport,
    uint16_t close_code,
    uint32_t timeout_ms);

wss_transport_result_t wss_transport_receive(
    wss_transport_t *transport,
    uint8_t *payload,
    size_t payload_capacity,
    wss_transport_message_t *message,
    uint32_t timeout_ms);

uint64_t wss_transport_last_receive_time_ms(const wss_transport_t *transport);

#endif
