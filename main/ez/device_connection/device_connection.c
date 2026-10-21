#include "device_connection.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "device_authentication.h"
#include "device_application_heartbeat.h"
#include "device_base64url.h"
#include "device_control_envelope.h"
#include "device_hardware_random.h"
#include "device_identity.h"
#include "device_media_control.h"
#include "device_message_replay_window.h"
#include "device_pairing.h"
#include "device_pcm_frame.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DEVICE_CONNECTION_TASK_STACK_BYTES 12288U
#define DEVICE_CONNECTION_TASK_PRIORITY 6U
#define DEVICE_CONNECTION_DISCOVERY_TIMEOUT_MS 5000U
#define DEVICE_CONNECTION_TCP_TIMEOUT_MS 5000U
#define DEVICE_CONNECTION_TLS_TIMEOUT_MS 10000U
#define DEVICE_CONNECTION_UPGRADE_TIMEOUT_MS 10000U
#define DEVICE_CONNECTION_RECEIVE_TIMEOUT_MS 30000U
#define DEVICE_CONNECTION_SEND_TIMEOUT_MS 5000U
#define DEVICE_CONNECTION_STOPPED_BIT BIT0
#define DEVICE_CONNECTION_MESSAGE_RANDOM_BYTES 18U
#define DEVICE_CONNECTION_MESSAGE_ID_BYTES 24U
#define DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES 2048U
#define DEVICE_CONNECTION_PAIRING_VALIDITY_MS 300000U
#define DEVICE_CONNECTION_ONLINE_POLL_MS 20U
#define DEVICE_CONNECTION_OUTBOUND_QUEUE_LENGTH 64U
#define DEVICE_CONNECTION_OUTBOUND_BURST 4U

typedef enum {
    DEVICE_CONNECTION_OUTBOUND_LISTEN_START = 0,
    DEVICE_CONNECTION_OUTBOUND_PCM,
    DEVICE_CONNECTION_OUTBOUND_LISTEN_STOP,
    DEVICE_CONNECTION_OUTBOUND_LISTEN_CANCEL,
    DEVICE_CONNECTION_OUTBOUND_PLAYBACK_CANCEL,
} device_connection_outbound_type_t;

typedef struct {
    device_connection_outbound_type_t type;
    uint64_t connection_epoch;
    union {
        struct {
            char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
            uint32_t stream_id;
        } listen_start;
        uint8_t pcm_frame[DEVICE_PCM_FRAME_BYTES];
        struct {
            uint32_t stream_id;
            uint32_t last_sequence;
        } listen_stop;
        struct {
            uint32_t stream_id;
        } listen_cancel;
        struct {
            char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
            uint32_t stream_id;
        } playback_cancel;
    } value;
} device_connection_outbound_t;

typedef struct {
    bool hello_sent;
    char device_id[DEVICE_IDENTITY_DEVICE_ID_MAX_BYTES + 1U];
    char connection_id[DEVICE_AUTHENTICATION_CONNECTION_ID_MAX_BYTES + 1U];
} device_connection_authentication_t;

typedef struct {
    bool network_has_ip;
    uint64_t network_ip_epoch;
    uint64_t identity_revision;
    bool stop_requested;
    device_connection_identity_t identity;
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY];
} device_connection_inputs_t;

struct device_connection {
    portMUX_TYPE input_lock;
    device_connection_inputs_t inputs;
    device_connection_event_callback_t event_callback;
    void *event_context;
    device_connection_media_callback_t media_callback;
    void *media_context;
    device_identity_t *identity_store;
    device_capabilities_t capabilities;
    device_output_preference_t output_preference;
    char client_version[DEVICE_AUTHENTICATION_CLIENT_VERSION_MAX_BYTES + 1U];
    TaskHandle_t task;
    EventGroupHandle_t lifecycle_events;
    QueueHandle_t outbound_queue;
    StaticQueue_t outbound_queue_state;
    uint8_t *outbound_queue_storage;
    bool started;
    device_connection_status_t status;
    device_connection_supervisor_t supervisor;
    wss_transport_t *transport;
    uint8_t *receive_payload;
    host_discovery_record_t cached_host;
    bool has_cached_host;
    uint64_t observed_ip_epoch;
    uint64_t observed_identity_revision;
    device_connection_identity_t identity;
    device_message_replay_window_t *replay_window;
    device_connection_authentication_t authentication;
    device_pairing_t *pairing;
    char *pairing_first_outbound;
    char *pairing_second_outbound;
    uint64_t pairing_deadline_ms;
};

static bool pairing_connection_lost(device_connection_t *connection);
static void receive_transport_message(device_connection_t *connection);

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static device_connection_inputs_t input_snapshot(device_connection_t *connection)
{
    device_connection_inputs_t snapshot;
    taskENTER_CRITICAL(&connection->input_lock);
    snapshot = connection->inputs;
    taskEXIT_CRITICAL(&connection->input_lock);
    return snapshot;
}

static bool outbound_identifier_valid(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, maximum_size + 1U);
    if (length == 0U || length > maximum_size) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!isspace((unsigned char)value[index])) {
            return true;
        }
    }
    return false;
}

static device_connection_result_t outbound_enqueue(
    device_connection_t *connection,
    const device_connection_outbound_t *outbound)
{
    TaskHandle_t task;
    bool online;
    taskENTER_CRITICAL(&connection->input_lock);
    online = connection->started &&
             connection->status.state == DEVICE_CONNECTION_STATE_ONLINE &&
             connection->status.connection_epoch == outbound->connection_epoch;
    task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (!online || task == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    if (xQueueSend(connection->outbound_queue, outbound, 0U) != pdTRUE) {
        return DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED;
    }
    xTaskNotifyGive(task);
    return DEVICE_CONNECTION_RESULT_OK;
}

static bool transport_cancelled(void *context)
{
    device_connection_t *connection = context;
    const device_connection_inputs_t inputs = input_snapshot(connection);
    return inputs.stop_requested || !inputs.network_has_ip ||
           inputs.network_ip_epoch != connection->observed_ip_epoch ||
           inputs.identity_revision != connection->observed_identity_revision;
}

static void copy_status(device_connection_t *connection)
{
    taskENTER_CRITICAL(&connection->input_lock);
    connection->status.state = connection->supervisor.state;
    connection->status.block_reason = connection->supervisor.block_reason;
    connection->status.connection_epoch = connection->supervisor.connection_epoch;
    connection->status.consecutive_failures = connection->supervisor.consecutive_failures;
    taskEXIT_CRITICAL(&connection->input_lock);
}

static void emit_event(
    device_connection_t *connection,
    device_connection_event_type_t type,
    device_connection_failure_t failure,
    host_selection_result_t selection_result,
    wss_transport_result_t transport_result,
    const char *message_id,
    const char *message_type)
{
    copy_status(connection);
    if (connection->event_callback == NULL) {
        return;
    }
    const device_connection_inputs_t inputs = input_snapshot(connection);
    device_connection_event_t event = {
        .type = type,
        .state = connection->supervisor.state,
        .block_reason = connection->supervisor.block_reason,
        .failure = failure,
        .selection_result = selection_result,
        .transport_result = transport_result,
        .network_ip_epoch = connection->observed_ip_epoch,
        .connection_epoch = connection->supervisor.connection_epoch,
        .paired = inputs.identity.paired,
        .pairing_state = connection->pairing == NULL
                             ? DEVICE_PAIRING_STATE_READY
                             : device_pairing_state(connection->pairing),
    };
    if (connection->supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF) {
        const uint64_t now_ms = monotonic_ms();
        if (connection->supervisor.backoff_until_ms > now_ms) {
            const uint64_t remaining = connection->supervisor.backoff_until_ms - now_ms;
            event.reconnect_delay_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        }
    }
    if (connection->has_cached_host) {
        const size_t installation_length = strlen(connection->cached_host.installation_id);
        if (installation_length >= sizeof(event.installation_id_suffix) - 1U) {
            memcpy(
                event.installation_id_suffix,
                connection->cached_host.installation_id + installation_length -
                    (sizeof(event.installation_id_suffix) - 1U),
                sizeof(event.installation_id_suffix) - 1U);
        }
    }
    if (message_id != NULL) {
        strncpy(event.message_id, message_id, sizeof(event.message_id) - 1U);
    }
    if (message_type != NULL) {
        strncpy(event.message_type, message_type, sizeof(event.message_type) - 1U);
    }
    connection->event_callback(&event, connection->event_context);
}

static void pairing_state_publish(
    device_connection_t *connection,
    bool pairing_code_changed)
{
    if (connection->pairing == NULL) {
        return;
    }
    const device_pairing_state_t state = device_pairing_state(connection->pairing);
    bool state_changed = false;
    taskENTER_CRITICAL(&connection->input_lock);
    if (connection->status.pairing_state != state) {
        connection->status.pairing_state = state;
        state_changed = true;
    }
    if (pairing_code_changed) {
        (void)device_pairing_copy_code(
            connection->pairing,
            connection->inputs.pairing_code);
    }
    taskEXIT_CRITICAL(&connection->input_lock);
    if (state_changed || pairing_code_changed) {
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_PAIRING_STATE_CHANGED,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            HOST_SELECTION_NOT_FOUND,
            WSS_TRANSPORT_RESULT_OK,
            NULL,
            NULL);
    }
}

static void emit_state_changed(device_connection_t *connection)
{
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_STATE_CHANGED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        HOST_SELECTION_NOT_FOUND,
        WSS_TRANSPORT_RESULT_OK,
        NULL,
        NULL);
}

static bool emit_media_event(
    device_connection_t *connection,
    const device_connection_media_event_t *event)
{
    return connection->media_callback == NULL ||
           connection->media_callback(event, connection->media_context);
}

static bool copy_transport_endpoint(
    const host_discovery_record_t *host,
    wss_transport_endpoint_t *endpoint)
{
    const size_t path_length = strlen(host->path);
    const size_t fingerprint_length = strlen(host->certificate_fingerprint);
    if (path_length >= sizeof(endpoint->path) ||
        fingerprint_length >= sizeof(endpoint->certificate_fingerprint)) {
        return false;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->ipv4_address = host->ipv4_address;
    endpoint->port = host->port;
    memcpy(endpoint->path, host->path, path_length + 1U);
    memcpy(
        endpoint->certificate_fingerprint,
        host->certificate_fingerprint,
        fingerprint_length + 1U);
    return true;
}

static device_connection_failure_t transport_failure(wss_transport_result_t result)
{
    switch (result) {
        case WSS_TRANSPORT_RESULT_CERTIFICATE_UNAVAILABLE:
        case WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH:
            return DEVICE_CONNECTION_FAILURE_CERTIFICATE;
        case WSS_TRANSPORT_RESULT_INVALID_ARGUMENT:
        case WSS_TRANSPORT_RESULT_UPGRADE_REJECTED:
        case WSS_TRANSPORT_RESULT_PROTOCOL_ERROR:
        case WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED:
            return DEVICE_CONNECTION_FAILURE_PROTOCOL;
        default:
            return DEVICE_CONNECTION_FAILURE_TRANSIENT;
    }
}

static void record_failure(
    device_connection_t *connection,
    device_connection_failure_t failure,
    wss_transport_result_t transport_result)
{
    const uint64_t epoch = connection->supervisor.connection_epoch;
    if (device_connection_supervisor_fail(
            &connection->supervisor,
            epoch,
            failure,
            monotonic_ms(),
            esp_random())) {
        ESP_LOGW("EZConnection", "Failure epoch=%llu category=%d transport=%d rx_idle_ms=%llu",
                 (unsigned long long)epoch, failure, transport_result,
                 (unsigned long long)(monotonic_ms() -
                     wss_transport_last_receive_time_ms(connection->transport)));
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            failure,
            HOST_SELECTION_NOT_FOUND,
            transport_result,
            NULL,
            NULL);
    }
}

static void record_discovery_failure(
    device_connection_t *connection,
    device_connection_failure_t failure,
    host_selection_result_t selection_result)
{
    if (device_connection_supervisor_discovery_failed(
            &connection->supervisor,
            failure,
            monotonic_ms(),
            esp_random())) {
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            failure,
            selection_result,
            WSS_TRANSPORT_RESULT_OK,
            NULL,
            NULL);
    }
}

static void synchronize_inputs(device_connection_t *connection)
{
    const device_connection_inputs_t inputs = input_snapshot(connection);
    const bool identity_changed =
        inputs.identity_revision != connection->observed_identity_revision;
    const bool network_changed =
        inputs.network_ip_epoch != connection->observed_ip_epoch;
    if (!inputs.network_has_ip || network_changed || identity_changed) {
        (void)pairing_connection_lost(connection);
        if (wss_transport_is_connected(connection->transport)) {
            ESP_LOGI("EZConnection", "Inputs has_ip=%d network_changed=%d identity_changed=%d",
                     inputs.network_has_ip, network_changed, identity_changed);
            wss_transport_disconnect(connection->transport);
        }
    }
    if (!inputs.network_has_ip) {
        if (connection->supervisor.wifi_has_ip) {
            device_connection_supervisor_wifi_down(&connection->supervisor, monotonic_ms());
            connection->has_cached_host = false;
            emit_state_changed(connection);
        }
        connection->observed_ip_epoch = inputs.network_ip_epoch;
        connection->observed_identity_revision = inputs.identity_revision;
        connection->identity = inputs.identity;
        return;
    }
    if (!connection->supervisor.wifi_has_ip || network_changed || identity_changed) {
        if (identity_changed) {
            device_connection_supervisor_identity_changed(&connection->supervisor);
        }
        if (connection->supervisor.wifi_has_ip) {
            device_connection_supervisor_wifi_down(&connection->supervisor, monotonic_ms());
        }
        connection->observed_ip_epoch = inputs.network_ip_epoch;
        connection->observed_identity_revision = inputs.identity_revision;
        connection->identity = inputs.identity;
        connection->has_cached_host = false;
        device_connection_supervisor_got_ip(&connection->supervisor, monotonic_ms());
        emit_state_changed(connection);
    }
}

static void discover_host(device_connection_t *connection)
{
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.discovery_attempt_count;
    taskEXIT_CRITICAL(&connection->input_lock);

    host_discovery_record_t records[HOST_DISCOVERY_MAX_RESULTS];
    size_t record_count = 0U;
    const host_discovery_result_t query_result = host_discovery_query(
        DEVICE_CONNECTION_DISCOVERY_TIMEOUT_MS,
        records,
        HOST_DISCOVERY_MAX_RESULTS,
        &record_count);
    if (transport_cancelled(connection)) {
        return;
    }
    if (query_result != HOST_DISCOVERY_RESULT_OK) {
        record_discovery_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            HOST_SELECTION_NOT_FOUND);
        return;
    }

    host_selection_identity_t selection_identity = {
        .paired = connection->identity.paired,
    };
    memcpy(
        selection_identity.installation_id,
        connection->identity.installation_id,
        sizeof(selection_identity.installation_id));
    memcpy(
        selection_identity.certificate_fingerprint,
        connection->identity.certificate_fingerprint,
        sizeof(selection_identity.certificate_fingerprint));
    host_discovery_record_t selected;
    const host_selection_result_t selection_result = host_discovery_select(
        records,
        record_count,
        &selection_identity,
        &selected);
    if (selection_result != HOST_SELECTION_SELECTED) {
        const device_connection_failure_t failure =
            selection_result == HOST_SELECTION_CERTIFICATE_CHANGED
                ? DEVICE_CONNECTION_FAILURE_CERTIFICATE
                : DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE;
        record_discovery_failure(connection, failure, selection_result);
        return;
    }

    connection->cached_host = selected;
    connection->has_cached_host = true;
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_HOST_SELECTED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        selection_result,
        WSS_TRANSPORT_RESULT_OK,
        NULL,
        NULL);
    uint64_t connection_epoch = 0U;
    if (device_connection_supervisor_connect_discovered(
            &connection->supervisor,
            &connection_epoch)) {
        device_message_replay_window_initialize(connection->replay_window);
        emit_state_changed(connection);
    }
}

static void connect_transport(device_connection_t *connection)
{
    if (!connection->has_cached_host) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
        return;
    }
    wss_transport_endpoint_t endpoint;
    if (!copy_transport_endpoint(&connection->cached_host, &endpoint)) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.transport_attempt_count;
    taskEXIT_CRITICAL(&connection->input_lock);
    const wss_transport_result_t result = wss_transport_connect(
        connection->transport,
        &endpoint,
        DEVICE_CONNECTION_TCP_TIMEOUT_MS,
        DEVICE_CONNECTION_TLS_TIMEOUT_MS,
        DEVICE_CONNECTION_UPGRADE_TIMEOUT_MS);
    if (result == WSS_TRANSPORT_RESULT_CANCELLED || transport_cancelled(connection)) {
        (void)pairing_connection_lost(connection);
        return;
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        if (!pairing_connection_lost(connection)) {
            record_failure(connection, transport_failure(result), result);
        }
        return;
    }
    memset(&connection->authentication, 0, sizeof(connection->authentication));
    if (device_connection_supervisor_transport_ready(
            &connection->supervisor,
            connection->supervisor.connection_epoch)) {
        emit_state_changed(connection);
    }
}

static bool create_random_token(
    size_t random_byte_count,
    char *encoded,
    size_t encoded_capacity)
{
    if (random_byte_count > DEVICE_AUTHENTICATION_NONCE_BYTES ||
        encoded == NULL || encoded_capacity == 0U) {
        return false;
    }
    uint8_t random_bytes[DEVICE_AUTHENTICATION_NONCE_BYTES];
    esp_fill_random(random_bytes, random_byte_count);
    size_t encoded_size = 0U;
    const device_crypto_result_t result = device_base64url_encode(
        random_bytes,
        random_byte_count,
        encoded,
        encoded_capacity - 1U,
        &encoded_size);
    memset(random_bytes, 0, sizeof(random_bytes));
    if (result != DEVICE_CRYPTO_RESULT_OK || encoded_size >= encoded_capacity) {
        return false;
    }
    encoded[encoded_size] = '\0';
    return true;
}

static bool authentication_identity_load(
    device_connection_t *connection,
    device_identity_record_t *identity)
{
    if (connection->identity_store == NULL ||
        device_identity_load(connection->identity_store, identity) !=
            DEVICE_IDENTITY_RESULT_OK ||
        (identity->state != DEVICE_IDENTITY_STATE_PENDING &&
         identity->state != DEVICE_IDENTITY_STATE_ACTIVE) ||
        strcmp(
            identity->host_installation_id,
            connection->cached_host.installation_id) != 0 ||
        strcmp(
            identity->host_certificate_fingerprint,
            connection->cached_host.certificate_fingerprint) != 0) {
        memset(identity, 0, sizeof(*identity));
        return false;
    }
    return true;
}

static device_connection_failure_t pairing_failure_type(
    device_pairing_result_t result)
{
    switch (result) {
        case DEVICE_PAIRING_RESULT_STORAGE_ERROR:
        case DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED:
            return DEVICE_CONNECTION_FAILURE_IDENTITY;
        case DEVICE_PAIRING_RESULT_NO_MEMORY:
            return DEVICE_CONNECTION_FAILURE_TRANSIENT;
        default:
            return DEVICE_CONNECTION_FAILURE_PROTOCOL;
    }
}

static bool pending_identity_adopt(device_connection_t *connection)
{
    if (connection->pairing == NULL || connection->identity_store == NULL ||
        device_pairing_state(connection->pairing) !=
            DEVICE_PAIRING_STATE_WAITING_FOR_COMPLETE) {
        return false;
    }
    device_identity_record_t record = {0};
    if (device_identity_load(connection->identity_store, &record) !=
            DEVICE_IDENTITY_RESULT_OK ||
        record.state != DEVICE_IDENTITY_STATE_PENDING) {
        memset(&record, 0, sizeof(record));
        return false;
    }
    device_connection_identity_t identity = {.paired = true};
    memcpy(
        identity.installation_id,
        record.host_installation_id,
        sizeof(identity.installation_id));
    memcpy(
        identity.certificate_fingerprint,
        record.host_certificate_fingerprint,
        sizeof(identity.certificate_fingerprint));
    memset(&record, 0, sizeof(record));
    return device_connection_update_identity(connection, &identity) ==
           DEVICE_CONNECTION_RESULT_OK;
}

static bool pairing_connection_lost(device_connection_t *connection)
{
    if (connection->identity.paired || connection->pairing == NULL) {
        return false;
    }
    if (pending_identity_adopt(connection)) {
        return true;
    }
    const uint64_t now_ms = monotonic_ms();
    const bool attempt_still_valid = connection->pairing_deadline_ms > now_ms;
    device_pairing_transition_t transition;
    (void)device_pairing_connection_lost(
        connection->pairing,
        attempt_still_valid,
        &transition);
    if (transition.attempt_reset) {
        connection->pairing_deadline_ms = 0U;
    }
    pairing_state_publish(connection, transition.pairing_code_rotated);
    return false;
}

static void pairing_challenge_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    if (connection->pairing == NULL) {
        return;
    }
    if (connection->pairing_first_outbound == NULL) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    device_authentication_challenge_t challenge = {0};
    if (device_authentication_decode_challenge(envelope, &challenge) !=
        DEVICE_AUTHENTICATION_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    memset(&challenge, 0, sizeof(challenge));
    if (connection->pairing_deadline_ms != 0U &&
        connection->pairing_deadline_ms <= monotonic_ms()) {
        device_pairing_transition_t transition;
        (void)device_pairing_connection_lost(connection->pairing, false, &transition);
        connection->pairing_deadline_ms = 0U;
        pairing_state_publish(connection, transition.pairing_code_rotated);
    }
    size_t encoded_size = 0U;
    const device_pairing_result_t pairing_result = device_pairing_begin(
        connection->pairing,
        &connection->cached_host,
        connection->pairing_first_outbound,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES,
        &encoded_size);
    if (pairing_result != DEVICE_PAIRING_RESULT_OK) {
        record_failure(
            connection,
            pairing_failure_type(pairing_result),
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    pairing_state_publish(connection, false);
    const wss_transport_result_t send_result = wss_transport_send_text(
        connection->transport,
        (const uint8_t *)connection->pairing_first_outbound,
        encoded_size,
        DEVICE_CONNECTION_SEND_TIMEOUT_MS);
    memset(
        connection->pairing_first_outbound,
        0,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES);
    if (send_result != WSS_TRANSPORT_RESULT_OK && !pairing_connection_lost(connection)) {
        record_failure(connection, transport_failure(send_result), send_result);
    }
}

static void authentication_challenge_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    device_authentication_challenge_t challenge = {0};
    if (connection->authentication.hello_sent ||
        device_authentication_decode_challenge(envelope, &challenge) !=
            DEVICE_AUTHENTICATION_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    device_identity_record_t identity = {0};
    if (!authentication_identity_load(connection, &identity)) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    char message_id[DEVICE_CONNECTION_MESSAGE_ID_BYTES + 1U];
    char device_nonce[DEVICE_AUTHENTICATION_NONCE_BASE64URL_BYTES + 1U];
    char encoded[DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES];
    size_t encoded_size = 0U;
    const bool tokens_created = create_random_token(
                                    DEVICE_CONNECTION_MESSAGE_RANDOM_BYTES,
                                    message_id,
                                    sizeof(message_id)) &&
                                create_random_token(
                                    DEVICE_AUTHENTICATION_NONCE_BYTES,
                                    device_nonce,
                                    sizeof(device_nonce));
    const device_authentication_result_t authentication_result = tokens_created
        ? device_authentication_encode_hello(
              message_id,
              &challenge,
              &identity,
              device_nonce,
              &connection->capabilities,
              connection->output_preference,
              connection->client_version,
              encoded,
              sizeof(encoded),
              &encoded_size)
        : DEVICE_AUTHENTICATION_RESULT_CRYPTOGRAPHY_FAILED;
    if (authentication_result != DEVICE_AUTHENTICATION_RESULT_OK) {
        memset(&identity, 0, sizeof(identity));
        memset(device_nonce, 0, sizeof(device_nonce));
        memset(encoded, 0, sizeof(encoded));
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    const wss_transport_result_t send_result = wss_transport_send_text(
        connection->transport,
        (const uint8_t *)encoded,
        encoded_size,
        DEVICE_CONNECTION_SEND_TIMEOUT_MS);
    if (send_result == WSS_TRANSPORT_RESULT_OK) {
        connection->authentication.hello_sent = true;
        memcpy(
            connection->authentication.device_id,
            identity.device_id,
            strlen(identity.device_id) + 1U);
        memcpy(
            connection->authentication.connection_id,
            challenge.connection_id,
            strlen(challenge.connection_id) + 1U);
    } else {
        record_failure(connection, transport_failure(send_result), send_result);
    }
    memset(&identity, 0, sizeof(identity));
    memset(device_nonce, 0, sizeof(device_nonce));
    memset(encoded, 0, sizeof(encoded));
}

static void authentication_ack_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    device_authentication_ack_t ack = {0};
    if (!connection->authentication.hello_sent ||
        device_authentication_decode_ack(
            envelope,
            connection->authentication.device_id,
            connection->authentication.connection_id,
            &ack) != DEVICE_AUTHENTICATION_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    device_identity_record_t identity = {0};
    if (!authentication_identity_load(connection, &identity)) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    if (identity.state == DEVICE_IDENTITY_STATE_PENDING &&
        device_identity_activate(connection->identity_store, identity.device_id) !=
            DEVICE_IDENTITY_RESULT_OK) {
        memset(&identity, 0, sizeof(identity));
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    memset(&identity, 0, sizeof(identity));
    if (device_connection_supervisor_authenticated(
            &connection->supervisor,
            connection->supervisor.connection_epoch,
            monotonic_ms())) {
        emit_state_changed(connection);
    }
}

static void clear_credential_and_discover(device_connection_t *connection)
{
    if (connection->identity_store == NULL ||
        device_identity_clear_credential(connection->identity_store, true) !=
            DEVICE_IDENTITY_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    bool pairing_code_changed = false;
    if (connection->pairing != NULL) {
        if (device_pairing_force_new_code(connection->pairing) !=
            DEVICE_PAIRING_RESULT_OK) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_IDENTITY,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        connection->pairing_deadline_ms = 0U;
        pairing_code_changed = true;
    }
    const device_connection_identity_t unpaired_identity = {0};
    if (device_connection_update_identity(connection, &unpaired_identity) !=
        DEVICE_CONNECTION_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_IDENTITY,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    if (pairing_code_changed) {
        pairing_state_publish(connection, true);
    }
}

static void pairing_control_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    if (connection->pairing == NULL || connection->pairing_first_outbound == NULL ||
        connection->pairing_second_outbound == NULL) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    device_pairing_transition_t transition;
    const device_pairing_result_t pairing_result = device_pairing_handle_control(
        connection->pairing,
        envelope,
        connection->pairing_first_outbound,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES,
        connection->pairing_second_outbound,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES,
        &transition);
    if (device_pairing_state(connection->pairing) ==
        DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL) {
        connection->pairing_deadline_ms = monotonic_ms() +
                                          DEVICE_CONNECTION_PAIRING_VALIDITY_MS;
    } else if (device_pairing_state(connection->pairing) >=
               DEVICE_PAIRING_STATE_WAITING_FOR_BIND_ACK) {
        connection->pairing_deadline_ms = 0U;
    }
    pairing_state_publish(connection, transition.pairing_code_rotated);
    if (pairing_result != DEVICE_PAIRING_RESULT_OK) {
        if (transition.pending_credential_saved && pending_identity_adopt(connection)) {
            return;
        }
        record_failure(
            connection,
            pairing_failure_type(pairing_result),
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    for (size_t index = 0U; index < transition.message_count; ++index) {
        char *outbound = index == 0U ? connection->pairing_first_outbound
                                    : connection->pairing_second_outbound;
        const wss_transport_result_t send_result = wss_transport_send_text(
            connection->transport,
            (const uint8_t *)outbound,
            transition.message_sizes[index],
            DEVICE_CONNECTION_SEND_TIMEOUT_MS);
        if (send_result != WSS_TRANSPORT_RESULT_OK) {
            memset(
                connection->pairing_first_outbound,
                0,
                DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES);
            memset(
                connection->pairing_second_outbound,
                0,
                DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES);
            if (!pairing_connection_lost(connection)) {
                record_failure(connection, transport_failure(send_result), send_result);
            }
            return;
        }
    }
    memset(
        connection->pairing_first_outbound,
        0,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES);
    memset(
        connection->pairing_second_outbound,
        0,
        DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES);
    if (transition.identity_activated) {
        device_identity_record_t record = {0};
        if (device_pairing_copy_identity(connection->pairing, &record) !=
            DEVICE_PAIRING_RESULT_OK) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_IDENTITY,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        device_connection_identity_t identity = {.paired = true};
        memcpy(
            identity.installation_id,
            record.host_installation_id,
            sizeof(identity.installation_id));
        memcpy(
            identity.certificate_fingerprint,
            record.host_certificate_fingerprint,
            sizeof(identity.certificate_fingerprint));
        memset(&record, 0, sizeof(record));
        if (device_connection_update_identity(connection, &identity) !=
            DEVICE_CONNECTION_RESULT_OK) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_IDENTITY,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        }
        return;
    }
    if (transition.attempt_reset) {
        connection->pairing_deadline_ms = 0U;
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
    }
}

static void authentication_error_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    device_authentication_error_t error = {0};
    if (device_authentication_decode_error(envelope, &error) !=
        DEVICE_AUTHENTICATION_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    if (strcmp(error.code, "device_revoked") == 0 && !error.recoverable) {
        clear_credential_and_discover(connection);
        return;
    }
    if (strcmp(error.code, "authentication_failed") == 0) {
        device_identity_record_t identity = {0};
        if (!authentication_identity_load(connection, &identity)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_IDENTITY,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        const bool pending = identity.state == DEVICE_IDENTITY_STATE_PENDING;
        memset(&identity, 0, sizeof(identity));
        if (pending) {
            clear_credential_and_discover(connection);
            return;
        }
    }
    record_failure(
        connection,
        strcmp(error.code, "authentication_failed") == 0
            ? DEVICE_CONNECTION_FAILURE_IDENTITY
            : error.recoverable ? DEVICE_CONNECTION_FAILURE_TRANSIENT
                                : DEVICE_CONNECTION_FAILURE_PROTOCOL,
        WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
}

static void online_control_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    const char *message_type = device_control_envelope_message_type(envelope);
    if (strcmp(message_type, "ping") == 0) {
        const uint64_t received_ms = monotonic_ms();
        ESP_LOGI("EZConnection", "Heartbeat ping epoch=%llu",
                 (unsigned long long)connection->supervisor.connection_epoch);
        device_application_heartbeat_t heartbeat = {0};
        char message_id[DEVICE_CONNECTION_MESSAGE_ID_BYTES + 1U] = {0};
        char encoded[DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES] = {0};
        size_t encoded_size = 0U;
        if (device_application_heartbeat_decode_ping(envelope, &heartbeat) !=
                DEVICE_PROTOCOL_RESULT_OK ||
            !create_random_token(
                DEVICE_CONNECTION_MESSAGE_RANDOM_BYTES,
                message_id,
                sizeof(message_id)) ||
            device_application_heartbeat_encode_pong(
                message_id,
                &heartbeat,
                encoded,
                sizeof(encoded),
                &encoded_size) != DEVICE_PROTOCOL_RESULT_OK) {
            memset(&heartbeat, 0, sizeof(heartbeat));
            memset(encoded, 0, sizeof(encoded));
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        const wss_transport_result_t send_result = wss_transport_send_text(
            connection->transport,
            (const uint8_t *)encoded,
            encoded_size,
            DEVICE_CONNECTION_SEND_TIMEOUT_MS);
        ESP_LOGI("EZConnection", "Heartbeat pong epoch=%llu result=%d elapsed_ms=%llu",
                 (unsigned long long)connection->supervisor.connection_epoch, send_result,
                 (unsigned long long)(monotonic_ms() - received_ms));
        memset(&heartbeat, 0, sizeof(heartbeat));
        memset(encoded, 0, sizeof(encoded));
        if (send_result != WSS_TRANSPORT_RESULT_OK) {
            record_failure(connection, transport_failure(send_result), send_result);
            return;
        }
    } else if (strcmp(message_type, "input_segment_accepted") == 0) {
        device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_INPUT_SEGMENT_ACCEPTED,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        if (device_media_control_decode_input_segment_accepted(
                envelope,
                &event.value.input_segment_accepted) != DEVICE_PROTOCOL_RESULT_OK ||
            !emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "input_accepted") == 0) {
        device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_INPUT_ACCEPTED,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        if (device_media_control_decode_input_accepted(
                envelope,
                &event.value.input_accepted) != DEVICE_PROTOCOL_RESULT_OK ||
            !emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "transcript") == 0 ||
               strcmp(message_type, "text_output") == 0) {
        const bool transcript = strcmp(message_type, "transcript") == 0;
        device_connection_media_event_t event = {
            .type = transcript ? DEVICE_CONNECTION_MEDIA_EVENT_TRANSCRIPT
                               : DEVICE_CONNECTION_MEDIA_EVENT_TEXT_OUTPUT,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        const device_protocol_result_t result =
            transcript
                ? device_media_control_decode_transcript(envelope, &event.value.transcript)
                : device_media_control_decode_text_output(envelope, &event.value.text_output);
        if (result != DEVICE_PROTOCOL_RESULT_OK || !emit_media_event(connection, &event)) {
            record_failure(connection, DEVICE_CONNECTION_FAILURE_PROTOCOL,
                           WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "state_changed") == 0) {
        device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        if (device_media_control_decode_state_changed(
                envelope,
                &event.value.state_changed) != DEVICE_PROTOCOL_RESULT_OK ||
            !emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "playback_start") == 0) {
        device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_START,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        if (device_media_control_decode_playback_start(
                envelope,
                &event.value.playback_start) != DEVICE_PROTOCOL_RESULT_OK ||
            !emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "playback_end") == 0) {
        device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END,
            .connection_epoch = connection->supervisor.connection_epoch,
        };
        if (device_media_control_decode_playback_end(
                envelope,
                &event.value.playback_end) != DEVICE_PROTOCOL_RESULT_OK ||
            !emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else if (strcmp(message_type, "error") == 0) {
        device_authentication_error_t error = {0};
        if (device_authentication_decode_error(envelope, &error) !=
            DEVICE_AUTHENTICATION_RESULT_OK) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        const device_connection_media_event_t event = {
            .type = DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR,
            .connection_epoch = connection->supervisor.connection_epoch,
            .value.remote_error = error,
        };
        if (!emit_media_event(connection, &event)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        if (strcmp(error.code, "device_revoked") == 0 && !error.recoverable) {
            clear_credential_and_discover(connection);
            return;
        }
        const bool input_stream_conflict = strcmp(error.code, "audio_input_busy") == 0 ||
                                           strcmp(error.code, "pcm_stream_mismatch") == 0;
        // A fresh authenticated connection discards the Host's conflicting input stream.
        if (!error.recoverable || input_stream_conflict) {
            const bool service_interruption = input_stream_conflict ||
                                              strcmp(error.code, "gateway_disabled") == 0 ||
                                              strcmp(error.code, "connection_replaced") == 0;
            record_failure(connection,
                           service_interruption ? DEVICE_CONNECTION_FAILURE_TRANSIENT
                                                : DEVICE_CONNECTION_FAILURE_PROTOCOL,
                           WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
    } else {
        record_failure(connection, DEVICE_CONNECTION_FAILURE_PROTOCOL,
                       WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    (void)device_connection_supervisor_receive_activity(
        &connection->supervisor,
        connection->supervisor.connection_epoch,
        monotonic_ms());
}

static void online_pcm_handle(
    device_connection_t *connection,
    size_t payload_length)
{
    device_connection_media_event_t event = {
        .type = DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME,
        .connection_epoch = connection->supervisor.connection_epoch,
    };
    if (device_pcm_frame_decode(
            connection->receive_payload,
            payload_length,
            DEVICE_PCM_DIRECTION_DOWNLINK,
            &event.value.pcm_frame) != DEVICE_PROTOCOL_RESULT_OK) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    if (!emit_media_event(connection, &event)) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED);
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.pcm_frame_count;
    taskEXIT_CRITICAL(&connection->input_lock);
    (void)device_connection_supervisor_receive_activity(
        &connection->supervisor,
        connection->supervisor.connection_epoch,
        monotonic_ms());
}

static bool outbound_process_one(
    device_connection_t *connection,
    bool *processed)
{
    device_connection_outbound_t outbound;
    if (xQueueReceive(connection->outbound_queue, &outbound, 0U) != pdTRUE) {
        *processed = false;
        return true;
    }
    *processed = true;
    if (connection->supervisor.state != DEVICE_CONNECTION_STATE_ONLINE ||
        outbound.connection_epoch != connection->supervisor.connection_epoch) {
        memset(&outbound, 0, sizeof(outbound));
        return true;
    }

    char message_id[DEVICE_CONNECTION_MESSAGE_ID_BYTES + 1U] = {0};
    char encoded[DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES] = {0};
    size_t encoded_size = 0U;
    device_protocol_result_t encode_result = DEVICE_PROTOCOL_RESULT_OK;
    wss_transport_result_t send_result;
    const bool pcm_outbound = outbound.type == DEVICE_CONNECTION_OUTBOUND_PCM;
    if (pcm_outbound) {
        send_result = wss_transport_send_binary(
            connection->transport,
            outbound.value.pcm_frame,
            sizeof(outbound.value.pcm_frame),
            DEVICE_CONNECTION_SEND_TIMEOUT_MS);
    } else {
        if (!create_random_token(
                DEVICE_CONNECTION_MESSAGE_RANDOM_BYTES,
                message_id,
                sizeof(message_id))) {
            memset(&outbound, 0, sizeof(outbound));
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return false;
        }
        switch (outbound.type) {
            case DEVICE_CONNECTION_OUTBOUND_LISTEN_START:
                encode_result = device_media_control_encode_listen_start(
                    message_id,
                    outbound.value.listen_start.client_input_id,
                    outbound.value.listen_start.stream_id,
                    connection->output_preference,
                    encoded,
                    sizeof(encoded),
                    &encoded_size);
                break;
            case DEVICE_CONNECTION_OUTBOUND_LISTEN_STOP:
                encode_result = device_media_control_encode_listen_stop(
                    message_id,
                    outbound.value.listen_stop.stream_id,
                    outbound.value.listen_stop.last_sequence,
                    encoded,
                    sizeof(encoded),
                    &encoded_size);
                break;
            case DEVICE_CONNECTION_OUTBOUND_LISTEN_CANCEL:
                encode_result = device_media_control_encode_listen_cancel(
                    message_id,
                    outbound.value.listen_cancel.stream_id,
                    encoded,
                    sizeof(encoded),
                    &encoded_size);
                break;
            case DEVICE_CONNECTION_OUTBOUND_PLAYBACK_CANCEL:
                encode_result = device_media_control_encode_playback_cancel(
                    message_id,
                    outbound.value.playback_cancel.output_id,
                    outbound.value.playback_cancel.stream_id,
                    encoded,
                    sizeof(encoded),
                    &encoded_size);
                break;
            default:
                encode_result = DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
                break;
        }
        if (encode_result != DEVICE_PROTOCOL_RESULT_OK) {
            memset(&outbound, 0, sizeof(outbound));
            memset(encoded, 0, sizeof(encoded));
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return false;
        }
        send_result = wss_transport_send_text(
            connection->transport,
            (const uint8_t *)encoded,
            encoded_size,
            DEVICE_CONNECTION_SEND_TIMEOUT_MS);
    }
    memset(&outbound, 0, sizeof(outbound));
    memset(encoded, 0, sizeof(encoded));
    if (send_result != WSS_TRANSPORT_RESULT_OK) {
        record_failure(connection, transport_failure(send_result), send_result);
        return false;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (pcm_outbound) {
        ++connection->status.outbound_pcm_frame_count;
    } else {
        ++connection->status.outbound_control_message_count;
    }
    taskEXIT_CRITICAL(&connection->input_lock);
    return true;
}

static void online_service(device_connection_t *connection)
{
    for (size_t index = 0U; index < DEVICE_CONNECTION_OUTBOUND_BURST; ++index) {
        bool processed = false;
        if (!outbound_process_one(connection, &processed) ||
            connection->supervisor.state != DEVICE_CONNECTION_STATE_ONLINE) {
            return;
        }
        if (!processed) {
            break;
        }
    }
    if (monotonic_ms() - wss_transport_last_receive_time_ms(connection->transport) >=
        DEVICE_CONNECTION_RECEIVE_TIMEOUT_MS) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            WSS_TRANSPORT_RESULT_TIMEOUT);
        return;
    }
    bool readable = false;
    const wss_transport_result_t wait_result = wss_transport_wait_readable(
        connection->transport,
        DEVICE_CONNECTION_ONLINE_POLL_MS,
        &readable);
    if (wait_result != WSS_TRANSPORT_RESULT_OK) {
        record_failure(connection, transport_failure(wait_result), wait_result);
        return;
    }
    if (readable) {
        receive_transport_message(connection);
    }
}

static void authentication_control_handle(
    device_connection_t *connection,
    const device_control_envelope_t *envelope)
{
    const char *message_type = device_control_envelope_message_type(envelope);
    if (strcmp(message_type, "auth_challenge") == 0) {
        if (connection->identity.paired && connection->identity_store != NULL) {
            authentication_challenge_handle(connection, envelope);
        } else {
            pairing_challenge_handle(connection, envelope);
        }
        return;
    }
    if (!connection->identity.paired) {
        pairing_control_handle(connection, envelope);
        return;
    }
    if (strcmp(message_type, "hello_ack") == 0) {
        authentication_ack_handle(connection, envelope);
        return;
    }
    if (strcmp(message_type, "error") == 0) {
        authentication_error_handle(connection, envelope);
        return;
    }
    if (connection->identity.paired && connection->identity_store != NULL) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
    }
}

static void receive_transport_message(device_connection_t *connection)
{
    wss_transport_message_t message;
    const wss_transport_result_t result = wss_transport_receive(
        connection->transport,
        connection->receive_payload,
        WSS_TRANSPORT_CONTROL_MAX_BYTES,
        &message,
        DEVICE_CONNECTION_RECEIVE_TIMEOUT_MS);
    if (result == WSS_TRANSPORT_RESULT_CANCELLED || transport_cancelled(connection)) {
        (void)pairing_connection_lost(connection);
        return;
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        if (!pairing_connection_lost(connection)) {
            record_failure(connection, transport_failure(result), result);
        }
        return;
    }
    if (message.type == WSS_TRANSPORT_MESSAGE_PING ||
        message.type == WSS_TRANSPORT_MESSAGE_PONG) {
        return;
    }
    if (message.type == WSS_TRANSPORT_MESSAGE_CLOSE) {
        if (!pairing_connection_lost(connection)) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_TRANSIENT,
                WSS_TRANSPORT_RESULT_CONNECTION_CLOSED);
        }
        return;
    }
    if (message.type == WSS_TRANSPORT_MESSAGE_BINARY) {
        if (connection->supervisor.state != DEVICE_CONNECTION_STATE_ONLINE) {
            record_failure(
                connection,
                DEVICE_CONNECTION_FAILURE_PROTOCOL,
                WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
            return;
        }
        online_pcm_handle(connection, message.payload_length);
        return;
    }
    if (message.type != WSS_TRANSPORT_MESSAGE_TEXT) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }

    device_control_envelope_t *envelope = NULL;
    const device_protocol_result_t decode_result = device_control_envelope_decode(
        (const char *)connection->receive_payload,
        message.payload_length,
        &envelope);
    if (decode_result != DEVICE_PROTOCOL_RESULT_OK || envelope == NULL ||
        device_message_replay_window_accept(
            connection->replay_window,
            device_control_envelope_message_id(envelope)) != DEVICE_PROTOCOL_RESULT_OK) {
        device_control_envelope_release(envelope);
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.control_message_count;
    taskEXIT_CRITICAL(&connection->input_lock);
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        HOST_SELECTION_SELECTED,
        WSS_TRANSPORT_RESULT_OK,
        device_control_envelope_message_id(envelope),
        device_control_envelope_message_type(envelope));
    if (connection->supervisor.state == DEVICE_CONNECTION_STATE_AUTHENTICATING) {
        authentication_control_handle(connection, envelope);
    } else if (connection->supervisor.state == DEVICE_CONNECTION_STATE_ONLINE) {
        online_control_handle(connection, envelope);
    }
    device_control_envelope_release(envelope);
}

static void wait_for_work(device_connection_t *connection)
{
    TickType_t wait_ticks = portMAX_DELAY;
    if (connection->supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF) {
        const uint64_t now_ms = monotonic_ms();
        if (connection->supervisor.backoff_until_ms <= now_ms) {
            wait_ticks = 0U;
        } else {
            const uint64_t remaining_ms = connection->supervisor.backoff_until_ms - now_ms;
            wait_ticks = pdMS_TO_TICKS(
                remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
        }
    }
    (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
}

static void device_connection_task(void *context)
{
    device_connection_t *connection = context;
    const host_discovery_result_t discovery_result = host_discovery_initialize();
    const wss_transport_result_t transport_result =
        discovery_result == HOST_DISCOVERY_RESULT_OK
            ? wss_transport_create(&connection->transport)
            : WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    connection->receive_payload = heap_caps_malloc(
        WSS_TRANSPORT_CONTROL_MAX_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    connection->replay_window = heap_caps_malloc(
        sizeof(device_message_replay_window_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (connection->pairing != NULL) {
        connection->pairing_first_outbound = heap_caps_malloc(
            DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        connection->pairing_second_outbound = heap_caps_malloc(
            DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const bool resources_ready = discovery_result == HOST_DISCOVERY_RESULT_OK &&
                                 transport_result == WSS_TRANSPORT_RESULT_OK &&
                                 connection->receive_payload != NULL &&
                                 connection->replay_window != NULL &&
                                 (connection->pairing == NULL ||
                                  (connection->pairing_first_outbound != NULL &&
                                   connection->pairing_second_outbound != NULL));
    if (!resources_ready) {
        connection->supervisor.state = DEVICE_CONNECTION_STATE_BLOCKED;
        connection->supervisor.block_reason = DEVICE_CONNECTION_BLOCK_PROTOCOL;
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            HOST_SELECTION_NOT_FOUND,
            transport_result,
            NULL,
            NULL);
    } else {
        wss_transport_set_cancel_callback(
            connection->transport,
            transport_cancelled,
            connection);
        pairing_state_publish(connection, true);
    }

    while (!resources_ready) {
        if (input_snapshot(connection).stop_requested) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    while (resources_ready) {
        const device_connection_inputs_t inputs = input_snapshot(connection);
        if (inputs.stop_requested) {
            break;
        }
        synchronize_inputs(connection);
        if (connection->supervisor.state != DEVICE_CONNECTION_STATE_ONLINE) {
            (void)xQueueReset(connection->outbound_queue);
        }
        if (!connection->supervisor.wifi_has_ip) {
            wait_for_work(connection);
            continue;
        }
        switch (connection->supervisor.state) {
            case DEVICE_CONNECTION_STATE_DISCOVERING:
                discover_host(connection);
                break;
            case DEVICE_CONNECTION_STATE_CONNECTING:
                connect_transport(connection);
                break;
            case DEVICE_CONNECTION_STATE_AUTHENTICATING:
                receive_transport_message(connection);
                break;
            case DEVICE_CONNECTION_STATE_ONLINE:
                online_service(connection);
                break;
            case DEVICE_CONNECTION_STATE_BACKOFF: {
                wait_for_work(connection);
                synchronize_inputs(connection);
                uint64_t connection_epoch = 0U;
                const device_connection_action_t action = device_connection_supervisor_poll(
                    &connection->supervisor,
                    monotonic_ms(),
                    esp_random(),
                    &connection_epoch);
                if (action != DEVICE_CONNECTION_ACTION_NONE) {
                    emit_state_changed(connection);
                }
                break;
            }
            case DEVICE_CONNECTION_STATE_BLOCKED:
                wait_for_work(connection);
                break;
            default:
                wait_for_work(connection);
                break;
        }
    }

    if (connection->transport != NULL) {
        wss_transport_destroy(connection->transport);
        connection->transport = NULL;
    }
    free(connection->receive_payload);
    connection->receive_payload = NULL;
    free(connection->replay_window);
    connection->replay_window = NULL;
    free(connection->pairing_first_outbound);
    connection->pairing_first_outbound = NULL;
    free(connection->pairing_second_outbound);
    connection->pairing_second_outbound = NULL;
    if (discovery_result == HOST_DISCOVERY_RESULT_OK) {
        host_discovery_deinitialize();
    }
    taskENTER_CRITICAL(&connection->input_lock);
    connection->started = false;
    connection->task = NULL;
    taskEXIT_CRITICAL(&connection->input_lock);
    xEventGroupSetBits(connection->lifecycle_events, DEVICE_CONNECTION_STOPPED_BIT);
    vTaskDelete(NULL);
}

static bool identity_valid(const device_connection_identity_t *identity)
{
    if (identity == NULL) {
        return false;
    }
    if (!identity->paired) {
        return identity->installation_id[0] == '\0' &&
               identity->certificate_fingerprint[0] == '\0';
    }
    host_selection_identity_t selection_identity = {.paired = true};
    memcpy(
        selection_identity.installation_id,
        identity->installation_id,
        sizeof(selection_identity.installation_id));
    memcpy(
        selection_identity.certificate_fingerprint,
        identity->certificate_fingerprint,
        sizeof(selection_identity.certificate_fingerprint));
    host_discovery_record_t selected;
    return host_discovery_select(NULL, 0U, &selection_identity, &selected) ==
           HOST_SELECTION_NOT_FOUND;
}

device_connection_result_t device_connection_create(
    const device_connection_config_t *config,
    device_connection_t **connection)
{
    if (config == NULL || connection == NULL || !identity_valid(&config->identity) ||
        (config->identity_store != NULL && config->display_name == NULL)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    *connection = calloc(1U, sizeof(device_connection_t));
    if (*connection == NULL) {
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    (*connection)->input_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    (*connection)->inputs.identity = config->identity;
    (*connection)->identity = config->identity;
    (*connection)->event_callback = config->event_callback;
    (*connection)->event_context = config->event_context;
    (*connection)->media_callback = config->media_callback;
    (*connection)->media_context = config->media_context;
    (*connection)->identity_store = config->identity_store;
    (*connection)->capabilities = config->capabilities;
    (*connection)->output_preference = config->output_preference;
    if (config->identity_store != NULL) {
        const device_pairing_config_t pairing_config = {
            .identity_store = config->identity_store,
            .capabilities = config->capabilities,
            .display_name = config->display_name,
            .random_source = device_hardware_random_source(),
        };
        const device_pairing_result_t pairing_result = device_pairing_create(
            &pairing_config,
            &(*connection)->pairing);
        if (pairing_result != DEVICE_PAIRING_RESULT_OK) {
            free(*connection);
            *connection = NULL;
            return pairing_result == DEVICE_PAIRING_RESULT_NO_MEMORY
                       ? DEVICE_CONNECTION_RESULT_NO_MEMORY
                       : DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
        }
        (void)device_pairing_copy_code(
            (*connection)->pairing,
            (*connection)->inputs.pairing_code);
    }
    if (config->client_version != NULL) {
        const size_t client_version_length = strlen(config->client_version);
        if (client_version_length == 0U ||
            client_version_length > DEVICE_AUTHENTICATION_CLIENT_VERSION_MAX_BYTES) {
            device_pairing_destroy((*connection)->pairing);
            free(*connection);
            *connection = NULL;
            return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
        }
        memcpy(
            (*connection)->client_version,
            config->client_version,
            client_version_length + 1U);
    }
    (*connection)->outbound_queue_storage = heap_caps_malloc(
        DEVICE_CONNECTION_OUTBOUND_QUEUE_LENGTH * sizeof(device_connection_outbound_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if ((*connection)->outbound_queue_storage != NULL) {
        (*connection)->outbound_queue = xQueueCreateStatic(
            DEVICE_CONNECTION_OUTBOUND_QUEUE_LENGTH,
            sizeof(device_connection_outbound_t),
            (*connection)->outbound_queue_storage,
            &(*connection)->outbound_queue_state);
    }
    if ((*connection)->outbound_queue == NULL) {
        free((*connection)->outbound_queue_storage);
        device_pairing_destroy((*connection)->pairing);
        free(*connection);
        *connection = NULL;
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    (*connection)->lifecycle_events = xEventGroupCreate();
    if ((*connection)->lifecycle_events == NULL) {
        vQueueDelete((*connection)->outbound_queue);
        free((*connection)->outbound_queue_storage);
        device_pairing_destroy((*connection)->pairing);
        free(*connection);
        *connection = NULL;
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    device_connection_supervisor_initialize(&(*connection)->supervisor);
    (*connection)->status.state = DEVICE_CONNECTION_STATE_WIFI_DOWN;
    (*connection)->status.pairing_state = (*connection)->pairing == NULL
                                               ? DEVICE_PAIRING_STATE_READY
                                               : device_pairing_state((*connection)->pairing);
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_start(device_connection_t *connection)
{
    if (connection == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (connection->started) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    connection->inputs.stop_requested = false;
    connection->started = true;
    taskEXIT_CRITICAL(&connection->input_lock);
    xEventGroupClearBits(connection->lifecycle_events, DEVICE_CONNECTION_STOPPED_BIT);
    if (xTaskCreate(
            device_connection_task,
            "device_connection",
            DEVICE_CONNECTION_TASK_STACK_BYTES,
            connection,
            DEVICE_CONNECTION_TASK_PRIORITY,
            &connection->task) != pdPASS) {
        taskENTER_CRITICAL(&connection->input_lock);
        connection->started = false;
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_network_changed(
    device_connection_t *connection,
    bool has_ip,
    uint64_t ip_epoch)
{
    if (connection == NULL || (has_ip && ip_epoch == 0U)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (!connection->started) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    connection->inputs.network_has_ip = has_ip;
    connection->inputs.network_ip_epoch = ip_epoch;
    connection->status.network_has_ip = has_ip;
    connection->status.network_ip_epoch = ip_epoch;
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_update_identity(
    device_connection_t *connection,
    const device_connection_identity_t *identity)
{
    if (connection == NULL || !identity_valid(identity)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (memcmp(&connection->inputs.identity, identity, sizeof(*identity)) == 0) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_OK;
    }
    connection->inputs.identity = *identity;
    ++connection->inputs.identity_revision;
    if (connection->inputs.identity_revision == 0U) {
        connection->inputs.identity_revision = 1U;
    }
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_copy_pairing_code(
    device_connection_t *connection,
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY])
{
    if (connection == NULL || pairing_code == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    if (connection->pairing == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    memcpy(
        pairing_code,
        connection->inputs.pairing_code,
        DEVICE_PAIRING_CODE_CAPACITY);
    taskEXIT_CRITICAL(&connection->input_lock);
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_send_listen_start(
    device_connection_t *connection,
    uint64_t connection_epoch,
    const char *client_input_id,
    uint32_t stream_id)
{
    if (connection == NULL || connection_epoch == 0U || stream_id == 0U ||
        !outbound_identifier_valid(client_input_id, DEVICE_CLIENT_INPUT_ID_MAX_BYTES)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    device_connection_outbound_t outbound = {
        .type = DEVICE_CONNECTION_OUTBOUND_LISTEN_START,
        .connection_epoch = connection_epoch,
        .value.listen_start.stream_id = stream_id,
    };
    memcpy(
        outbound.value.listen_start.client_input_id,
        client_input_id,
        strlen(client_input_id) + 1U);
    const device_connection_result_t result = outbound_enqueue(connection, &outbound);
    memset(&outbound, 0, sizeof(outbound));
    return result;
}

device_connection_result_t device_connection_send_pcm(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id,
    uint32_t sequence,
    const uint8_t pcm_payload[DEVICE_PCM_PAYLOAD_BYTES])
{
    if (connection == NULL || connection_epoch == 0U || stream_id == 0U ||
        pcm_payload == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    device_connection_outbound_t outbound = {
        .type = DEVICE_CONNECTION_OUTBOUND_PCM,
        .connection_epoch = connection_epoch,
    };
    if (device_pcm_frame_encode(
            DEVICE_PCM_DIRECTION_UPLINK,
            stream_id,
            sequence,
            pcm_payload,
            DEVICE_PCM_PAYLOAD_BYTES,
            outbound.value.pcm_frame,
            sizeof(outbound.value.pcm_frame)) != DEVICE_PROTOCOL_RESULT_OK) {
        memset(&outbound, 0, sizeof(outbound));
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    const device_connection_result_t result = outbound_enqueue(connection, &outbound);
    memset(&outbound, 0, sizeof(outbound));
    return result;
}

device_connection_result_t device_connection_send_listen_stop(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id,
    uint32_t last_sequence)
{
    if (connection == NULL || connection_epoch == 0U || stream_id == 0U) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    device_connection_outbound_t outbound = {
        .type = DEVICE_CONNECTION_OUTBOUND_LISTEN_STOP,
        .connection_epoch = connection_epoch,
        .value.listen_stop = {
            .stream_id = stream_id,
            .last_sequence = last_sequence,
        },
    };
    return outbound_enqueue(connection, &outbound);
}

device_connection_result_t device_connection_send_listen_cancel(
    device_connection_t *connection,
    uint64_t connection_epoch,
    uint32_t stream_id)
{
    if (connection == NULL || connection_epoch == 0U || stream_id == 0U) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    const device_connection_outbound_t outbound = {
        .type = DEVICE_CONNECTION_OUTBOUND_LISTEN_CANCEL,
        .connection_epoch = connection_epoch,
        .value.listen_cancel.stream_id = stream_id,
    };
    return outbound_enqueue(connection, &outbound);
}

device_connection_result_t device_connection_send_playback_cancel(
    device_connection_t *connection,
    uint64_t connection_epoch,
    const char *output_id,
    uint32_t stream_id)
{
    if (connection == NULL || connection_epoch == 0U || stream_id == 0U ||
        !outbound_identifier_valid(output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    device_connection_outbound_t outbound = {
        .type = DEVICE_CONNECTION_OUTBOUND_PLAYBACK_CANCEL,
        .connection_epoch = connection_epoch,
        .value.playback_cancel.stream_id = stream_id,
    };
    memcpy(
        outbound.value.playback_cancel.output_id,
        output_id,
        strlen(output_id) + 1U);
    const device_connection_result_t result = outbound_enqueue(connection, &outbound);
    memset(&outbound, 0, sizeof(outbound));
    return result;
}

void device_connection_get_status(
    device_connection_t *connection,
    device_connection_status_t *status)
{
    if (connection == NULL || status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    *status = connection->status;
    taskEXIT_CRITICAL(&connection->input_lock);
}

void device_connection_stop(device_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    const bool started = connection->started;
    connection->inputs.stop_requested = true;
    connection->inputs.network_has_ip = false;
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (!started) {
        return;
    }
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    (void)xEventGroupWaitBits(
        connection->lifecycle_events,
        DEVICE_CONNECTION_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void device_connection_destroy(device_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    device_connection_stop(connection);
    device_pairing_destroy(connection->pairing);
    vQueueDelete(connection->outbound_queue);
    free(connection->outbound_queue_storage);
    vEventGroupDelete(connection->lifecycle_events);
    free(connection);
}
