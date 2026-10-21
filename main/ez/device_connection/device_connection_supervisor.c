#include "device_connection_supervisor.h"

#include <stddef.h>
#include <string.h>

static const uint32_t BACKOFF_INTERVALS_MS[] = {1000U, 2000U, 4000U, 8000U, 15000U};
static const uint32_t SERVICE_BACKOFF_INTERVALS_MS[] = {2000U, 4000U, 8000U, 10000U};

static uint64_t next_epoch(uint64_t current)
{
    ++current;
    return current == 0U ? 1U : current;
}

static uint32_t backoff_interval_from(
    const uint32_t *intervals,
    size_t interval_count,
    uint32_t consecutive_failures,
    uint32_t jitter_sample)
{
    size_t index = consecutive_failures == 0U ? 0U : consecutive_failures - 1U;
    if (index >= interval_count) {
        index = interval_count - 1U;
    }
    const uint32_t scale_per_thousand = 800U + (jitter_sample % 401U);
    return (intervals[index] * scale_per_thousand) / 1000U;
}

static uint32_t backoff_interval(
    device_connection_failure_t failure,
    uint32_t consecutive_failures,
    uint32_t jitter_sample)
{
    if (failure == DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE) {
        return backoff_interval_from(
            SERVICE_BACKOFF_INTERVALS_MS,
            sizeof(SERVICE_BACKOFF_INTERVALS_MS) /
                sizeof(SERVICE_BACKOFF_INTERVALS_MS[0]),
            consecutive_failures,
            jitter_sample);
    }
    return backoff_interval_from(
        BACKOFF_INTERVALS_MS,
        sizeof(BACKOFF_INTERVALS_MS) / sizeof(BACKOFF_INTERVALS_MS[0]),
        consecutive_failures,
        jitter_sample);
}

static bool epoch_is_active(
    const device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch)
{
    return connection_epoch != 0U && connection_epoch == supervisor->connection_epoch &&
           supervisor->wifi_has_ip;
}

static bool begin_connection(
    device_connection_supervisor_t *supervisor,
    uint64_t *connection_epoch)
{
    if (!supervisor->wifi_has_ip || supervisor->state == DEVICE_CONNECTION_STATE_CONNECTING ||
        supervisor->state == DEVICE_CONNECTION_STATE_AUTHENTICATING ||
        supervisor->state == DEVICE_CONNECTION_STATE_ONLINE ||
        supervisor->state == DEVICE_CONNECTION_STATE_BLOCKED || connection_epoch == NULL) {
        return false;
    }
    supervisor->connection_epoch = next_epoch(supervisor->connection_epoch);
    supervisor->failure_recorded_epoch = 0U;
    supervisor->state = DEVICE_CONNECTION_STATE_CONNECTING;
    *connection_epoch = supervisor->connection_epoch;
    return true;
}

static void enter_backoff(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms,
    uint32_t delay_ms,
    device_connection_action_t next_action)
{
    supervisor->state = DEVICE_CONNECTION_STATE_BACKOFF;
    supervisor->backoff_until_ms = now_ms + delay_ms;
    supervisor->action_after_backoff = next_action;
}

void device_connection_supervisor_initialize(device_connection_supervisor_t *supervisor)
{
    if (supervisor == NULL) {
        return;
    }
    memset(supervisor, 0, sizeof(*supervisor));
    supervisor->state = DEVICE_CONNECTION_STATE_WIFI_DOWN;
}

void device_connection_supervisor_wifi_down(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms)
{
    (void)now_ms;
    if (supervisor == NULL) {
        return;
    }
    supervisor->wifi_has_ip = false;
    supervisor->connection_epoch = next_epoch(supervisor->connection_epoch);
    supervisor->failure_recorded_epoch = 0U;
    supervisor->state = DEVICE_CONNECTION_STATE_WIFI_DOWN;
    supervisor->backoff_until_ms = 0U;
    supervisor->action_after_backoff = DEVICE_CONNECTION_ACTION_NONE;
    supervisor->cached_endpoint_retry_available = false;
}

void device_connection_supervisor_got_ip(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms)
{
    (void)now_ms;
    if (supervisor == NULL) {
        return;
    }
    supervisor->wifi_has_ip = true;
    supervisor->connection_epoch = next_epoch(supervisor->connection_epoch);
    supervisor->failure_recorded_epoch = 0U;
    supervisor->state = supervisor->block_reason == DEVICE_CONNECTION_BLOCK_NONE
                            ? DEVICE_CONNECTION_STATE_DISCOVERING
                            : DEVICE_CONNECTION_STATE_BLOCKED;
    supervisor->backoff_until_ms = 0U;
    supervisor->action_after_backoff = DEVICE_CONNECTION_ACTION_NONE;
    supervisor->cached_endpoint_retry_available = false;
}

void device_connection_supervisor_identity_changed(
    device_connection_supervisor_t *supervisor)
{
    if (supervisor == NULL) {
        return;
    }
    supervisor->connection_epoch = next_epoch(supervisor->connection_epoch);
    supervisor->failure_recorded_epoch = 0U;
    supervisor->block_reason = DEVICE_CONNECTION_BLOCK_NONE;
    supervisor->duplicate_count = 0U;
    supervisor->duplicate_window_started_ms = 0U;
    supervisor->consecutive_failures = 0U;
    supervisor->backoff_until_ms = 0U;
    supervisor->action_after_backoff = DEVICE_CONNECTION_ACTION_NONE;
    supervisor->cached_endpoint_retry_available = false;
    supervisor->state = supervisor->wifi_has_ip ? DEVICE_CONNECTION_STATE_DISCOVERING
                                                 : DEVICE_CONNECTION_STATE_WIFI_DOWN;
}

bool device_connection_supervisor_connect_discovered(
    device_connection_supervisor_t *supervisor,
    uint64_t *connection_epoch)
{
    if (supervisor == NULL || supervisor->state != DEVICE_CONNECTION_STATE_DISCOVERING) {
        return false;
    }
    supervisor->cached_endpoint_retry_available = true;
    return begin_connection(supervisor, connection_epoch);
}

bool device_connection_supervisor_discovery_failed(
    device_connection_supervisor_t *supervisor,
    device_connection_failure_t failure,
    uint64_t now_ms,
    uint32_t jitter_sample)
{
    if (supervisor == NULL || supervisor->state != DEVICE_CONNECTION_STATE_DISCOVERING ||
        failure == DEVICE_CONNECTION_FAILURE_DUPLICATE) {
        return false;
    }
    supervisor->connection_epoch = next_epoch(supervisor->connection_epoch);
    supervisor->failure_recorded_epoch = 0U;
    supervisor->cached_endpoint_retry_available = false;
    return device_connection_supervisor_fail(
        supervisor,
        supervisor->connection_epoch,
        failure,
        now_ms,
        jitter_sample);
}

bool device_connection_supervisor_transport_ready(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch)
{
    if (supervisor == NULL || !epoch_is_active(supervisor, connection_epoch) ||
        supervisor->state != DEVICE_CONNECTION_STATE_CONNECTING) {
        return false;
    }
    supervisor->state = DEVICE_CONNECTION_STATE_AUTHENTICATING;
    return true;
}

bool device_connection_supervisor_authenticated(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    uint64_t now_ms)
{
    if (supervisor == NULL || !epoch_is_active(supervisor, connection_epoch) ||
        supervisor->state != DEVICE_CONNECTION_STATE_AUTHENTICATING) {
        return false;
    }
    supervisor->state = DEVICE_CONNECTION_STATE_ONLINE;
    supervisor->online_since_ms = now_ms;
    supervisor->last_receive_ms = now_ms;
    return true;
}

bool device_connection_supervisor_receive_activity(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    uint64_t now_ms)
{
    if (supervisor == NULL || !epoch_is_active(supervisor, connection_epoch) ||
        supervisor->state != DEVICE_CONNECTION_STATE_ONLINE) {
        return false;
    }
    supervisor->last_receive_ms = now_ms;
    if (now_ms - supervisor->online_since_ms >= DEVICE_CONNECTION_STABLE_INTERVAL_MS) {
        supervisor->consecutive_failures = 0U;
    }
    return true;
}

bool device_connection_supervisor_fail(
    device_connection_supervisor_t *supervisor,
    uint64_t connection_epoch,
    device_connection_failure_t failure,
    uint64_t now_ms,
    uint32_t jitter_sample)
{
    if (supervisor == NULL || !epoch_is_active(supervisor, connection_epoch) ||
        supervisor->state == DEVICE_CONNECTION_STATE_WIFI_DOWN ||
        supervisor->state == DEVICE_CONNECTION_STATE_BLOCKED ||
        supervisor->failure_recorded_epoch == connection_epoch) {
        return false;
    }
    supervisor->failure_recorded_epoch = connection_epoch;

    if (failure == DEVICE_CONNECTION_FAILURE_CERTIFICATE ||
        failure == DEVICE_CONNECTION_FAILURE_IDENTITY ||
        failure == DEVICE_CONNECTION_FAILURE_PROTOCOL) {
        supervisor->state = DEVICE_CONNECTION_STATE_BLOCKED;
        supervisor->block_reason = failure == DEVICE_CONNECTION_FAILURE_CERTIFICATE
                                       ? DEVICE_CONNECTION_BLOCK_CERTIFICATE
                                   : failure == DEVICE_CONNECTION_FAILURE_IDENTITY
                                       ? DEVICE_CONNECTION_BLOCK_IDENTITY
                                       : DEVICE_CONNECTION_BLOCK_PROTOCOL;
        return true;
    }

    if (failure == DEVICE_CONNECTION_FAILURE_DUPLICATE) {
        if (supervisor->duplicate_count == 0U ||
            now_ms - supervisor->duplicate_window_started_ms > 120000U) {
            supervisor->duplicate_window_started_ms = now_ms;
            supervisor->duplicate_count = 1U;
        } else {
            ++supervisor->duplicate_count;
        }
        if (supervisor->duplicate_count >= 3U) {
            supervisor->state = DEVICE_CONNECTION_STATE_BLOCKED;
            supervisor->block_reason = DEVICE_CONNECTION_BLOCK_DUPLICATE;
            return true;
        }
        enter_backoff(supervisor, now_ms, 30000U, DEVICE_CONNECTION_ACTION_DISCOVER);
        return true;
    }

    if (supervisor->consecutive_failures < UINT32_MAX) {
        ++supervisor->consecutive_failures;
    }
    const device_connection_action_t action = supervisor->cached_endpoint_retry_available
                                                  ? DEVICE_CONNECTION_ACTION_CONNECT_CACHED
                                                  : DEVICE_CONNECTION_ACTION_DISCOVER;
    enter_backoff(
        supervisor,
        now_ms,
        backoff_interval(failure, supervisor->consecutive_failures, jitter_sample),
        action);
    return true;
}

device_connection_action_t device_connection_supervisor_poll(
    device_connection_supervisor_t *supervisor,
    uint64_t now_ms,
    uint32_t jitter_sample,
    uint64_t *connection_epoch)
{
    if (supervisor == NULL || !supervisor->wifi_has_ip || connection_epoch == NULL) {
        return DEVICE_CONNECTION_ACTION_NONE;
    }
    if (supervisor->state == DEVICE_CONNECTION_STATE_ONLINE) {
        if (now_ms - supervisor->online_since_ms >= DEVICE_CONNECTION_STABLE_INTERVAL_MS) {
            supervisor->consecutive_failures = 0U;
        }
        if (now_ms - supervisor->last_receive_ms >= DEVICE_CONNECTION_HEARTBEAT_TIMEOUT_MS) {
            const uint64_t epoch = supervisor->connection_epoch;
            (void)device_connection_supervisor_fail(
                supervisor,
                epoch,
                DEVICE_CONNECTION_FAILURE_TRANSIENT,
                now_ms,
                jitter_sample);
        }
    }
    if (supervisor->state != DEVICE_CONNECTION_STATE_BACKOFF ||
        now_ms < supervisor->backoff_until_ms) {
        return DEVICE_CONNECTION_ACTION_NONE;
    }

    const device_connection_action_t action = supervisor->action_after_backoff;
    supervisor->action_after_backoff = DEVICE_CONNECTION_ACTION_NONE;
    if (action == DEVICE_CONNECTION_ACTION_CONNECT_CACHED) {
        supervisor->cached_endpoint_retry_available = false;
        return begin_connection(supervisor, connection_epoch)
                   ? DEVICE_CONNECTION_ACTION_CONNECT_CACHED
                   : DEVICE_CONNECTION_ACTION_NONE;
    }
    supervisor->state = DEVICE_CONNECTION_STATE_DISCOVERING;
    return DEVICE_CONNECTION_ACTION_DISCOVER;
}
