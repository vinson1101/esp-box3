#ifndef DEVICE_CONNECTION_SUPERVISOR_H
#define DEVICE_CONNECTION_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_CONNECTION_HEARTBEAT_TIMEOUT_MS 30000U
#define DEVICE_CONNECTION_STABLE_INTERVAL_MS 60000U

typedef enum {
    DEVICE_CONNECTION_STATE_WIFI_DOWN = 0,
    DEVICE_CONNECTION_STATE_DISCOVERING,
    DEVICE_CONNECTION_STATE_CONNECTING,
    DEVICE_CONNECTION_STATE_AUTHENTICATING,
    DEVICE_CONNECTION_STATE_ONLINE,
    DEVICE_CONNECTION_STATE_BACKOFF,
    DEVICE_CONNECTION_STATE_BLOCKED,
} device_connection_state_t;

typedef enum {
    DEVICE_CONNECTION_FAILURE_TRANSIENT = 0,
    DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE,
    DEVICE_CONNECTION_FAILURE_CERTIFICATE,
    DEVICE_CONNECTION_FAILURE_IDENTITY,
    DEVICE_CONNECTION_FAILURE_PROTOCOL,
    DEVICE_CONNECTION_FAILURE_DUPLICATE,
} device_connection_failure_t;

typedef enum {
    DEVICE_CONNECTION_BLOCK_NONE = 0,
    DEVICE_CONNECTION_BLOCK_CERTIFICATE,
    DEVICE_CONNECTION_BLOCK_IDENTITY,
    DEVICE_CONNECTION_BLOCK_PROTOCOL,
    DEVICE_CONNECTION_BLOCK_DUPLICATE,
} device_connection_block_t;

typedef enum {
    DEVICE_CONNECTION_ACTION_NONE = 0,
    DEVICE_CONNECTION_ACTION_DISCOVER,
    DEVICE_CONNECTION_ACTION_CONNECT_CACHED,
} device_connection_action_t;

typedef struct {
    device_connection_state_t state;
    device_connection_block_t block_reason;
    uint64_t connection_epoch;
    uint64_t failure_recorded_epoch;
    uint64_t online_since_ms;
    uint64_t last_receive_ms;
    uint64_t backoff_until_ms;
    uint64_t duplicate_window_started_ms;
    uint32_t consecutive_failures;
    uint8_t duplicate_count;
    bool wifi_has_ip;
    bool cached_endpoint_retry_available;
    device_connection_action_t action_after_backoff;
} device_connection_supervisor_t;

void device_connection_supervisor_initialize(device_connection_supervisor_t *supervisor);

void device_connection_supervisor_wifi_down(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms);

void device_connection_supervisor_got_ip(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms);

void device_connection_supervisor_identity_changed(
    device_connection_supervisor_t *supervisor);

bool device_connection_supervisor_connect_discovered(
    device_connection_supervisor_t *supervisor,
    uint64_t *connection_epoch);

bool device_connection_supervisor_discovery_failed(
    device_connection_supervisor_t *supervisor,
    device_connection_failure_t failure,
    uint64_t now_ms,
    uint32_t jitter_sample);

bool device_connection_supervisor_transport_ready(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch);

bool device_connection_supervisor_authenticated(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    uint64_t now_ms);

bool device_connection_supervisor_receive_activity(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    uint64_t now_ms);

bool device_connection_supervisor_fail(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    device_connection_failure_t failure,
    uint64_t now_ms,
    uint32_t jitter_sample);

device_connection_action_t device_connection_supervisor_poll(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms,
    uint32_t jitter_sample,
    uint64_t *connection_epoch);

#endif
