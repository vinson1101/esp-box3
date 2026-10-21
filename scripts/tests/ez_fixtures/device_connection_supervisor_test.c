#include "device_connection_supervisor.h"

#include <assert.h>
#include <stdio.h>

static uint64_t begin_connection(device_connection_supervisor_t *supervisor)
{
    uint64_t epoch = 0U;
    assert(supervisor->state == DEVICE_CONNECTION_STATE_DISCOVERING);
    assert(device_connection_supervisor_connect_discovered(supervisor, &epoch));
    assert(epoch != 0U);
    return epoch;
}

static void verify_backoff_schedule(void)
{
    static const uint32_t expected_base_ms[] = {1000U, 2000U, 4000U, 8000U, 15000U, 15000U};
    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    device_connection_supervisor_got_ip(&supervisor, 0U);

    uint64_t now_ms = 0U;
    for (size_t index = 0U; index < sizeof(expected_base_ms) / sizeof(expected_base_ms[0]); index += 2U) {
        uint64_t epoch = begin_connection(&supervisor);
        assert(device_connection_supervisor_fail(
            &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, now_ms, 0U));
        assert(supervisor.backoff_until_ms - now_ms == expected_base_ms[index] * 8U / 10U);
        now_ms = supervisor.backoff_until_ms;
        assert(device_connection_supervisor_poll(&supervisor, now_ms, 0U, &epoch) ==
               DEVICE_CONNECTION_ACTION_CONNECT_CACHED);
        assert(device_connection_supervisor_fail(
            &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, now_ms, 400U));
        assert(supervisor.backoff_until_ms - now_ms == expected_base_ms[index + 1U] * 12U / 10U);
        now_ms = supervisor.backoff_until_ms;
        assert(device_connection_supervisor_poll(&supervisor, now_ms, 0U, &epoch) ==
               DEVICE_CONNECTION_ACTION_DISCOVER);
    }
}

static void verify_stable_connection_reset(void)
{
    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    device_connection_supervisor_got_ip(&supervisor, 0U);
    uint64_t epoch = begin_connection(&supervisor);
    assert(device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 0U, 200U));
    assert(device_connection_supervisor_poll(
               &supervisor, supervisor.backoff_until_ms, 200U, &epoch) ==
           DEVICE_CONNECTION_ACTION_CONNECT_CACHED);
    assert(device_connection_supervisor_transport_ready(&supervisor, epoch));
    assert(device_connection_supervisor_authenticated(&supervisor, epoch, 1000U));
    assert(device_connection_supervisor_receive_activity(&supervisor, epoch, 61000U));
    assert(supervisor.consecutive_failures == 0U);
    assert(device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 61001U, 200U));
    assert(supervisor.backoff_until_ms == 62001U);
}

static void verify_discovery_backoff_schedule(void)
{
    static const uint32_t expected_base_ms[] = {2000U, 4000U, 8000U, 10000U, 10000U};
    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    device_connection_supervisor_got_ip(&supervisor, 0U);

    uint64_t now_ms = 0U;
    for (size_t index = 0U; index < sizeof(expected_base_ms) / sizeof(expected_base_ms[0]);
         ++index) {
        assert(device_connection_supervisor_discovery_failed(
            &supervisor,
            DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE,
            now_ms,
            200U));
        assert(supervisor.backoff_until_ms - now_ms == expected_base_ms[index]);
        now_ms = supervisor.backoff_until_ms;
        uint64_t epoch = 0U;
        assert(device_connection_supervisor_poll(&supervisor, now_ms, 200U, &epoch) ==
               DEVICE_CONNECTION_ACTION_DISCOVER);
    }
}

int main(void)
{
    verify_backoff_schedule();
    verify_stable_connection_reset();
    verify_discovery_backoff_schedule();

    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_WIFI_DOWN);

    uint64_t epoch = 0U;
    device_connection_supervisor_got_ip(&supervisor, 0U);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_DISCOVERING);
    assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(epoch != 0U);
    assert(!device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(device_connection_supervisor_transport_ready(&supervisor, epoch));
    assert(device_connection_supervisor_authenticated(&supervisor, epoch, 100U));

    assert(device_connection_supervisor_receive_activity(&supervisor, epoch, 15100U));
    assert(device_connection_supervisor_poll(&supervisor, 45099U, 200U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);
    assert(device_connection_supervisor_poll(&supervisor, 45100U, 200U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF);
    const uint64_t first_backoff = supervisor.backoff_until_ms;
    assert(first_backoff == 46100U);

    assert(!device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 45101U, 200U));
    assert(supervisor.backoff_until_ms == first_backoff);
    assert(device_connection_supervisor_poll(&supervisor, first_backoff, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_CONNECT_CACHED);
    const uint64_t retry_epoch = epoch;
    assert(device_connection_supervisor_fail(
        &supervisor, retry_epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 47000U, 0U));
    assert(device_connection_supervisor_poll(
               &supervisor, supervisor.backoff_until_ms, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_DISCOVER);

    assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_CERTIFICATE, 50000U, 0U));
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BLOCKED);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_CERTIFICATE);
    assert(device_connection_supervisor_poll(&supervisor, 100000U, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);

    device_connection_supervisor_wifi_down(&supervisor, 100001U);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_WIFI_DOWN);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_CERTIFICATE);
    assert(!device_connection_supervisor_transport_ready(&supervisor, epoch));
    device_connection_supervisor_got_ip(&supervisor, 100002U);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BLOCKED);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_CERTIFICATE);
    device_connection_supervisor_identity_changed(&supervisor);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_DISCOVERING);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_NONE);

    for (size_t duplicate = 0U; duplicate < 3U; ++duplicate) {
        assert(supervisor.state == DEVICE_CONNECTION_STATE_DISCOVERING);
        assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
        assert(device_connection_supervisor_fail(
            &supervisor,
            epoch,
            DEVICE_CONNECTION_FAILURE_DUPLICATE,
            110000U + duplicate * 30000U,
            0U));
        if (duplicate < 2U) {
            assert(device_connection_supervisor_poll(
                       &supervisor, supervisor.backoff_until_ms, 0U, &epoch) ==
                   DEVICE_CONNECTION_ACTION_DISCOVER);
        }
    }
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BLOCKED);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_DUPLICATE);

    puts("device connection supervisor tests passed");
    return 0;
}
