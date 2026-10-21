#include "wss_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "mbedtls/md.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "wss_frame_codec.h"
#include "wss_upgrade.h"

#define WSS_TRANSPORT_UPGRADE_REQUEST_MAX_BYTES 512U
#define WSS_TRANSPORT_PCM_MESSAGE_BYTES 656U
#define WSS_TRANSPORT_CANCEL_POLL_INTERVAL_MS 100U

struct wss_transport {
    int socket_fd;
    mbedtls_ssl_context tls;
    mbedtls_ssl_config tls_config;
    bool tls_initialized;
    bool connected;
    bool close_sent;
    uint8_t received[WSS_UPGRADE_HEADER_MAX_BYTES];
    size_t received_offset;
    size_t received_length;
    uint8_t *fragment_payload;
    size_t fragment_length;
    uint64_t last_receive_time_ms;
    wss_transport_cancel_callback_t cancel_callback;
    void *cancel_context;
};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static uint64_t deadline_after(uint32_t timeout_ms)
{
    return monotonic_ms() + timeout_ms;
}

static bool deadline_remaining(uint64_t deadline, struct timeval *timeout)
{
    const uint64_t now = monotonic_ms();
    if (now >= deadline) {
        return false;
    }
    const uint64_t remaining_ms = deadline - now;
    timeout->tv_sec = (time_t)(remaining_ms / 1000U);
    timeout->tv_usec = (suseconds_t)((remaining_ms % 1000U) * 1000U);
    return true;
}

static bool operation_cancelled(const wss_transport_t *transport)
{
    return transport->cancel_callback != NULL &&
           transport->cancel_callback(transport->cancel_context);
}

static wss_transport_result_t wait_for_socket(
    wss_transport_t *transport,
    bool write,
    uint64_t deadline)
{
    while (true) {
        if (operation_cancelled(transport)) {
            return WSS_TRANSPORT_RESULT_CANCELLED;
        }
        struct timeval timeout;
        if (!deadline_remaining(deadline, &timeout)) {
            return WSS_TRANSPORT_RESULT_TIMEOUT;
        }
        const uint64_t remaining_us =
            (uint64_t)timeout.tv_sec * 1000000U + (uint64_t)timeout.tv_usec;
        const uint64_t poll_interval_us =
            (uint64_t)WSS_TRANSPORT_CANCEL_POLL_INTERVAL_MS * 1000U;
        if (remaining_us > poll_interval_us) {
            timeout.tv_sec = 0;
            timeout.tv_usec = (suseconds_t)poll_interval_us;
        }
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(transport->socket_fd, write ? &write_set : &read_set);
        const int result = select(
            transport->socket_fd + 1,
            write ? NULL : &read_set,
            write ? &write_set : NULL,
            NULL,
            &timeout);
        if (result > 0) {
            return WSS_TRANSPORT_RESULT_OK;
        }
        if (result == 0) {
            continue;
        }
        if (errno != EINTR) {
            ESP_LOGW("EZTransport", "Socket wait failed write=%d errno=%d", write, errno);
            return WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
        }
    }
}

static int tls_socket_send(void *context, const unsigned char *buffer, size_t length)
{
    const int socket_fd = *(const int *)context;
    const ssize_t result = send(socket_fd, buffer, length, 0);
    if (result >= 0) {
        return (int)result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_socket_receive(void *context, unsigned char *buffer, size_t length)
{
    const int socket_fd = *(const int *)context;
    const ssize_t result = recv(socket_fd, buffer, length, 0);
    if (result >= 0) {
        return (int)result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void release_fragment(wss_transport_t *transport)
{
    free(transport->fragment_payload);
    transport->fragment_payload = NULL;
    transport->fragment_length = 0U;
}

static void close_connection(wss_transport_t *transport)
{
    transport->connected = false;
    transport->close_sent = false;
    transport->received_offset = 0U;
    transport->received_length = 0U;
    release_fragment(transport);
    if (transport->socket_fd >= 0) {
        shutdown(transport->socket_fd, SHUT_RDWR);
        close(transport->socket_fd);
        transport->socket_fd = -1;
    }
    if (transport->tls_initialized) {
        mbedtls_ssl_free(&transport->tls);
        mbedtls_ssl_config_free(&transport->tls_config);
        transport->tls_initialized = false;
    }
}

static wss_transport_result_t connect_socket(
    wss_transport_t *transport,
    const wss_transport_endpoint_t *endpoint,
    uint64_t deadline)
{
    transport->socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (transport->socket_fd < 0) {
        return WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    }
    const int flags = fcntl(transport->socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(transport->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    }

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(endpoint->port),
        .sin_addr.s_addr = endpoint->ipv4_address,
    };
    if (connect(
            transport->socket_fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        return WSS_TRANSPORT_RESULT_OK;
    }
    if (errno != EINPROGRESS) {
        return WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    }
    const wss_transport_result_t wait_result =
        wait_for_socket(transport, true, deadline);
    if (wait_result != WSS_TRANSPORT_RESULT_OK) {
        return wait_result;
    }
    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    if (getsockopt(
            transport->socket_fd,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &socket_error_length) != 0 ||
        socket_error != 0) {
        return WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    }
    return WSS_TRANSPORT_RESULT_OK;
}

static wss_transport_result_t tls_handshake(wss_transport_t *transport, uint64_t deadline)
{
    mbedtls_ssl_init(&transport->tls);
    mbedtls_ssl_config_init(&transport->tls_config);
    transport->tls_initialized = true;
    if (mbedtls_ssl_config_defaults(
            &transport->tls_config,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return WSS_TRANSPORT_RESULT_TLS_FAILED;
    }
    mbedtls_ssl_conf_authmode(&transport->tls_config, MBEDTLS_SSL_VERIFY_NONE);
    if (mbedtls_ssl_setup(&transport->tls, &transport->tls_config) != 0) {
        return WSS_TRANSPORT_RESULT_TLS_FAILED;
    }
    mbedtls_ssl_set_bio(
        &transport->tls,
        &transport->socket_fd,
        tls_socket_send,
        tls_socket_receive,
        NULL);

    while (true) {
        if (operation_cancelled(transport)) {
            return WSS_TRANSPORT_RESULT_CANCELLED;
        }
        const int result = mbedtls_ssl_handshake(&transport->tls);
        if (result == 0) {
            return WSS_TRANSPORT_RESULT_OK;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW("EZTransport", "TLS handshake failed: -0x%04x", (unsigned int)-result);
            return WSS_TRANSPORT_RESULT_TLS_FAILED;
        }
        const wss_transport_result_t wait_result = wait_for_socket(
            transport, result == MBEDTLS_ERR_SSL_WANT_WRITE, deadline);
        if (wait_result != WSS_TRANSPORT_RESULT_OK) {
            return wait_result;
        }
    }
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_fingerprint(const char *text, uint8_t output[32U])
{
    if (text == NULL || strlen(text) != WSS_TRANSPORT_FINGERPRINT_LENGTH) {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        const int high = hex_nibble(text[index * 2U]);
        const int low = hex_nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool fixed_text_is_terminated(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static wss_transport_result_t verify_certificate_pin(
    wss_transport_t *transport,
    const char *expected_fingerprint)
{
    const mbedtls_x509_crt *certificate = mbedtls_ssl_get_peer_cert(&transport->tls);
    if (certificate == NULL || certificate->raw.p == NULL || certificate->raw.len == 0U) {
        return WSS_TRANSPORT_RESULT_CERTIFICATE_UNAVAILABLE;
    }
    uint8_t expected[32U];
    if (!decode_fingerprint(expected_fingerprint, expected)) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t actual[32U];
    if (sha256 == NULL ||
        mbedtls_md(sha256, certificate->raw.p, certificate->raw.len, actual) != 0) {
        return WSS_TRANSPORT_RESULT_TLS_FAILED;
    }
    uint8_t difference = 0U;
    for (size_t index = 0U; index < sizeof(actual); ++index) {
        difference |= actual[index] ^ expected[index];
    }
    memset(expected, 0, sizeof(expected));
    memset(actual, 0, sizeof(actual));
    return difference == 0U ? WSS_TRANSPORT_RESULT_OK
                            : WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH;
}

static wss_transport_result_t tls_write_all(
    wss_transport_t *transport,
    const uint8_t *data,
    size_t length,
    uint64_t deadline)
{
    size_t offset = 0U;
    while (offset < length) {
        if (operation_cancelled(transport)) {
            return WSS_TRANSPORT_RESULT_CANCELLED;
        }
        const int result = mbedtls_ssl_write(&transport->tls, &data[offset], length - offset);
        const int saved_errno = errno;
        if (result > 0) {
            offset += (size_t)result;
            continue;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW("EZTransport", "TLS write closed result=%d errno_snapshot=%d",
                     result, saved_errno);
            return WSS_TRANSPORT_RESULT_CONNECTION_CLOSED;
        }
        const wss_transport_result_t wait_result = wait_for_socket(
            transport, result == MBEDTLS_ERR_SSL_WANT_WRITE, deadline);
        if (wait_result != WSS_TRANSPORT_RESULT_OK) {
            return wait_result;
        }
    }
    return WSS_TRANSPORT_RESULT_OK;
}

static wss_transport_result_t tls_read_some(
    wss_transport_t *transport,
    uint8_t *data,
    size_t capacity,
    size_t *length,
    uint64_t deadline)
{
    while (true) {
        if (operation_cancelled(transport)) {
            return WSS_TRANSPORT_RESULT_CANCELLED;
        }
        const int result = mbedtls_ssl_read(&transport->tls, data, capacity);
        const int saved_errno = errno;
        if (result > 0) {
            *length = (size_t)result;
            return WSS_TRANSPORT_RESULT_OK;
        }
        if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            ESP_LOGW("EZTransport", "TLS peer closed result=%d errno_snapshot=%d",
                     result, saved_errno);
            return WSS_TRANSPORT_RESULT_CONNECTION_CLOSED;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW("EZTransport", "TLS read closed result=%d errno_snapshot=%d",
                     result, saved_errno);
            return WSS_TRANSPORT_RESULT_CONNECTION_CLOSED;
        }
        const wss_transport_result_t wait_result = wait_for_socket(
            transport, result == MBEDTLS_ERR_SSL_WANT_WRITE, deadline);
        if (wait_result != WSS_TRANSPORT_RESULT_OK) {
            return wait_result;
        }
    }
}

static wss_transport_result_t buffered_read_exact(
    wss_transport_t *transport,
    uint8_t *output,
    size_t length,
    uint64_t deadline)
{
    size_t copied = 0U;
    while (copied < length) {
        if (transport->received_offset < transport->received_length) {
            const size_t available = transport->received_length - transport->received_offset;
            const size_t take = available < length - copied ? available : length - copied;
            memcpy(output + copied, transport->received + transport->received_offset, take);
            transport->received_offset += take;
            copied += take;
            continue;
        }
        transport->received_offset = 0U;
        transport->received_length = 0U;
        size_t received = 0U;
        const wss_transport_result_t result = tls_read_some(
            transport,
            transport->received,
            sizeof(transport->received),
            &received,
            deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            return result;
        }
        transport->received_length = received;
    }
    return WSS_TRANSPORT_RESULT_OK;
}

static wss_transport_result_t websocket_upgrade(
    wss_transport_t *transport,
    const wss_transport_endpoint_t *endpoint,
    uint64_t deadline)
{
    uint8_t random_key[WSS_CLIENT_KEY_BYTES];
    esp_fill_random(random_key, sizeof(random_key));
    char client_key[WSS_CLIENT_KEY_TEXT_CAPACITY];
    char expected_accept[WSS_ACCEPT_TEXT_CAPACITY];
    if (wss_client_key_encode(random_key, client_key) != WSS_CODEC_RESULT_OK ||
        wss_upgrade_expected_accept(client_key, expected_accept) != WSS_CODEC_RESULT_OK) {
        return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
    }
    memset(random_key, 0, sizeof(random_key));

    struct in_addr address = {.s_addr = endpoint->ipv4_address};
    char host[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &address, host, sizeof(host)) == NULL) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    uint8_t request[WSS_TRANSPORT_UPGRADE_REQUEST_MAX_BYTES];
    size_t request_length = 0U;
    if (wss_upgrade_build_request(
            host,
            endpoint->port,
            endpoint->path,
            client_key,
            request,
            sizeof(request),
            &request_length) != WSS_CODEC_RESULT_OK) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    wss_transport_result_t result =
        tls_write_all(transport, request, request_length, deadline);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        return result;
    }

    size_t header_length = 0U;
    while (true) {
        const wss_codec_result_t end_result = wss_upgrade_find_header_end(
            transport->received, transport->received_length, &header_length);
        if (end_result == WSS_CODEC_RESULT_OK) {
            break;
        }
        if (end_result != WSS_CODEC_RESULT_INCOMPLETE ||
            transport->received_length == sizeof(transport->received)) {
            return WSS_TRANSPORT_RESULT_UPGRADE_REJECTED;
        }
        size_t received = 0U;
        result = tls_read_some(
            transport,
            transport->received + transport->received_length,
            sizeof(transport->received) - transport->received_length,
            &received,
            deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            return result;
        }
        transport->received_length += received;
    }
    if (wss_upgrade_validate_response(
            transport->received, header_length, expected_accept) != WSS_CODEC_RESULT_OK) {
        return WSS_TRANSPORT_RESULT_UPGRADE_REJECTED;
    }
    transport->received_offset = header_length;
    return WSS_TRANSPORT_RESULT_OK;
}

static bool valid_utf8(const uint8_t *value, size_t length)
{
    size_t index = 0U;
    while (index < length) {
        const uint8_t first = value[index++];
        if (first <= 0x7fU) {
            continue;
        }
        size_t continuation_count = 0U;
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > length - index) {
            return false;
        }
        for (size_t continuation = 0U; continuation < continuation_count; ++continuation) {
            const uint8_t byte = value[index++];
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool valid_close_code(uint16_t close_code)
{
    return close_code >= 1000U && close_code <= 4999U && close_code != 1004U &&
           close_code != 1005U && close_code != 1006U && close_code != 1015U;
}

static wss_transport_result_t send_frame(
    wss_transport_t *transport,
    wss_opcode_t opcode,
    const uint8_t *payload,
    size_t payload_length,
    uint64_t deadline)
{
    if (transport == NULL || !transport->connected ||
        (payload == NULL && payload_length != 0U)) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    if (payload_length > SIZE_MAX - WSS_FRAME_HEADER_MAX_BYTES) {
        return WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED;
    }
    uint8_t *frame = malloc(payload_length + WSS_FRAME_HEADER_MAX_BYTES);
    if (frame == NULL) {
        return WSS_TRANSPORT_RESULT_NO_MEMORY;
    }
    uint8_t mask[4U];
    esp_fill_random(mask, sizeof(mask));
    size_t frame_length = 0U;
    const wss_codec_result_t encode_result = wss_frame_encode_client(
        opcode,
        true,
        mask,
        payload,
        payload_length,
        frame,
        payload_length + WSS_FRAME_HEADER_MAX_BYTES,
        &frame_length);
    if (encode_result != WSS_CODEC_RESULT_OK) {
        free(frame);
        return encode_result == WSS_CODEC_RESULT_CAPACITY_EXCEEDED
                   ? WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED
                   : WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
    }
    const wss_transport_result_t result =
        tls_write_all(transport, frame, frame_length, deadline);
    free(frame);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        close_connection(transport);
    }
    return result;
}

wss_transport_result_t wss_transport_create(wss_transport_t **transport)
{
    if (transport == NULL) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    *transport = calloc(1U, sizeof(wss_transport_t));
    if (*transport == NULL) {
        return WSS_TRANSPORT_RESULT_NO_MEMORY;
    }
    (*transport)->socket_fd = -1;
    return WSS_TRANSPORT_RESULT_OK;
}

void wss_transport_set_cancel_callback(
    wss_transport_t *transport,
    wss_transport_cancel_callback_t callback,
    void *context)
{
    if (transport != NULL) {
        transport->cancel_callback = callback;
        transport->cancel_context = context;
    }
}

void wss_transport_destroy(wss_transport_t *transport)
{
    if (transport == NULL) {
        return;
    }
    close_connection(transport);
    free(transport);
}

wss_transport_result_t wss_transport_connect(
    wss_transport_t *transport,
    const wss_transport_endpoint_t *endpoint,
    uint32_t tcp_timeout_ms,
    uint32_t tls_timeout_ms,
    uint32_t upgrade_timeout_ms)
{
    uint8_t fingerprint[32U];
    if (transport == NULL || endpoint == NULL || endpoint->ipv4_address == 0U ||
        endpoint->port == 0U ||
        !fixed_text_is_terminated(endpoint->path, sizeof(endpoint->path)) ||
        endpoint->path[0] != '/' ||
        !fixed_text_is_terminated(
            endpoint->certificate_fingerprint, sizeof(endpoint->certificate_fingerprint)) ||
        !decode_fingerprint(endpoint->certificate_fingerprint, fingerprint) ||
        tcp_timeout_ms == 0U || tls_timeout_ms == 0U || upgrade_timeout_ms == 0U) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    memset(fingerprint, 0, sizeof(fingerprint));
    close_connection(transport);

    wss_transport_result_t result = connect_socket(
        transport, endpoint, deadline_after(tcp_timeout_ms));
    ESP_LOGI("EZTransport", "TCP ipv4=0x%08lx port=%u result=%d",
             (unsigned long)endpoint->ipv4_address, endpoint->port, result);
    if (result == WSS_TRANSPORT_RESULT_OK) {
        result = tls_handshake(transport, deadline_after(tls_timeout_ms));
    }
    if (result == WSS_TRANSPORT_RESULT_OK) {
        result = verify_certificate_pin(transport, endpoint->certificate_fingerprint);
    }
    if (result == WSS_TRANSPORT_RESULT_OK) {
        result = websocket_upgrade(transport, endpoint, deadline_after(upgrade_timeout_ms));
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        ESP_LOGW("EZTransport", "Connection failed result=%d", result);
        close_connection(transport);
        return result;
    }
    transport->connected = true;
    transport->last_receive_time_ms = monotonic_ms();
    return WSS_TRANSPORT_RESULT_OK;
}

void wss_transport_disconnect(wss_transport_t *transport)
{
    if (transport != NULL) {
        close_connection(transport);
    }
}

bool wss_transport_is_connected(const wss_transport_t *transport)
{
    return transport != NULL && transport->connected;
}

wss_transport_result_t wss_transport_wait_readable(
    wss_transport_t *transport,
    uint32_t timeout_ms,
    bool *readable)
{
    if (transport == NULL || !transport->connected || timeout_ms == 0U || readable == NULL) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    if (transport->received_offset < transport->received_length ||
        mbedtls_ssl_check_pending(&transport->tls) != 0) {
        *readable = true;
        return WSS_TRANSPORT_RESULT_OK;
    }
    const wss_transport_result_t result =
        wait_for_socket(transport, false, deadline_after(timeout_ms));
    if (result == WSS_TRANSPORT_RESULT_TIMEOUT) {
        *readable = false;
        return WSS_TRANSPORT_RESULT_OK;
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        return result;
    }
    *readable = true;
    return WSS_TRANSPORT_RESULT_OK;
}

wss_transport_result_t wss_transport_send_text(
    wss_transport_t *transport,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t timeout_ms)
{
    if (payload_length > WSS_TRANSPORT_CONTROL_MAX_BYTES || timeout_ms == 0U ||
        !valid_utf8(payload, payload_length)) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    return send_frame(
        transport, WSS_OPCODE_TEXT, payload, payload_length, deadline_after(timeout_ms));
}

wss_transport_result_t wss_transport_send_binary(
    wss_transport_t *transport,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t timeout_ms)
{
    if (payload_length != WSS_TRANSPORT_PCM_MESSAGE_BYTES || timeout_ms == 0U) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    return send_frame(
        transport, WSS_OPCODE_BINARY, payload, payload_length, deadline_after(timeout_ms));
}

wss_transport_result_t wss_transport_send_close(
    wss_transport_t *transport,
    uint16_t close_code,
    uint32_t timeout_ms)
{
    if (transport == NULL || !valid_close_code(close_code) || timeout_ms == 0U) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    const uint8_t payload[2U] = {(uint8_t)(close_code >> 8U), (uint8_t)close_code};
    ESP_LOGI("EZTransport", "WebSocket local close code=%u", (unsigned)close_code);
    const wss_transport_result_t result = send_frame(
        transport, WSS_OPCODE_CLOSE, payload, sizeof(payload), deadline_after(timeout_ms));
    if (result == WSS_TRANSPORT_RESULT_OK) {
        transport->close_sent = true;
    }
    return result;
}

static wss_transport_result_t read_frame_header(
    wss_transport_t *transport,
    wss_frame_header_t *header,
    uint64_t deadline)
{
    uint8_t bytes[10U];
    wss_transport_result_t result = buffered_read_exact(transport, bytes, 2U, deadline);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        return result;
    }
    size_t header_length = 2U;
    const uint8_t marker = bytes[1] & 0x7fU;
    if (marker == 126U) {
        header_length = 4U;
    } else if (marker == 127U) {
        header_length = 10U;
    }
    if (header_length > 2U) {
        result = buffered_read_exact(
            transport, &bytes[2U], header_length - 2U, deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            return result;
        }
    }
    return wss_frame_decode_server_header(bytes, header_length, header) == WSS_CODEC_RESULT_OK
               ? WSS_TRANSPORT_RESULT_OK
               : WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
}

static wss_transport_result_t deliver_control_frame(
    wss_transport_t *transport,
    const wss_frame_header_t *header,
    uint8_t *payload,
    size_t payload_capacity,
    wss_transport_message_t *message,
    uint64_t deadline)
{
    if (header->payload_length > payload_capacity) {
        return WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED;
    }
    const size_t length = (size_t)header->payload_length;
    wss_transport_result_t result =
        buffered_read_exact(transport, payload, length, deadline);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        return result;
    }
    message->payload_length = length;
    message->close_code = 0U;
    if (header->opcode == WSS_OPCODE_PING) {
        const uint64_t received_ms = monotonic_ms();
        transport->last_receive_time_ms = monotonic_ms();
        result = send_frame(transport, WSS_OPCODE_PONG, payload, length, deadline);
        ESP_LOGI("EZTransport", "WebSocket ping bytes=%u pong_result=%d elapsed_ms=%llu",
                 (unsigned)length, result, (unsigned long long)(monotonic_ms() - received_ms));
        if (result != WSS_TRANSPORT_RESULT_OK) {
            return result;
        }
        message->type = WSS_TRANSPORT_MESSAGE_PING;
        return WSS_TRANSPORT_RESULT_OK;
    }
    if (header->opcode == WSS_OPCODE_PONG) {
        ESP_LOGI("EZTransport", "WebSocket pong bytes=%u", (unsigned)length);
        transport->last_receive_time_ms = monotonic_ms();
        message->type = WSS_TRANSPORT_MESSAGE_PONG;
        return WSS_TRANSPORT_RESULT_OK;
    }

    if (length == 1U || (length > 2U && !valid_utf8(&payload[2U], length - 2U))) {
        return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
    }
    message->type = WSS_TRANSPORT_MESSAGE_CLOSE;
    if (length >= 2U) {
        message->close_code = ((uint16_t)payload[0] << 8U) | payload[1];
        if (!valid_close_code(message->close_code)) {
            return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
        }
    }
    ESP_LOGW("EZTransport", "WebSocket close code=%u reason=%.*s", (unsigned)message->close_code,
             (int)(length > 2U ? length - 2U : 0U), length > 2U ? (const char *)&payload[2U] : "");
    transport->last_receive_time_ms = monotonic_ms();
    if (!transport->close_sent) {
        result = send_frame(transport, WSS_OPCODE_CLOSE, payload, length, deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            return result;
        }
    }
    transport->connected = false;
    return WSS_TRANSPORT_RESULT_OK;
}

wss_transport_result_t wss_transport_receive(
    wss_transport_t *transport,
    uint8_t *payload,
    size_t payload_capacity,
    wss_transport_message_t *message,
    uint32_t timeout_ms)
{
    if (transport == NULL || !transport->connected || payload == NULL || message == NULL ||
        timeout_ms == 0U) {
        return WSS_TRANSPORT_RESULT_INVALID_ARGUMENT;
    }
    const uint64_t deadline = deadline_after(timeout_ms);
receive_next_frame:
    wss_frame_header_t header;
    wss_transport_result_t result = read_frame_header(transport, &header, deadline);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        close_connection(transport);
        return result;
    }

    if (header.opcode == WSS_OPCODE_PING || header.opcode == WSS_OPCODE_PONG ||
        header.opcode == WSS_OPCODE_CLOSE) {
        result = deliver_control_frame(
            transport, &header, payload, payload_capacity, message, deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            close_connection(transport);
        }
        return result;
    }

    if (header.payload_length > SIZE_MAX) {
        close_connection(transport);
        return WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED;
    }
    const size_t frame_length = (size_t)header.payload_length;
    if (header.opcode == WSS_OPCODE_CONTINUATION) {
        if (transport->fragment_payload == NULL ||
            frame_length > WSS_TRANSPORT_CONTROL_MAX_BYTES - transport->fragment_length) {
            close_connection(transport);
            return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
        }
        result = buffered_read_exact(
            transport,
            &transport->fragment_payload[transport->fragment_length],
            frame_length,
            deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            close_connection(transport);
            return result;
        }
        transport->fragment_length += frame_length;
        transport->last_receive_time_ms = monotonic_ms();
        if (!header.final) {
            goto receive_next_frame;
        }
        if (transport->fragment_length > payload_capacity) {
            close_connection(transport);
            return WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED;
        }
        if (!valid_utf8(transport->fragment_payload, transport->fragment_length)) {
            close_connection(transport);
            return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
        }
        memcpy(payload, transport->fragment_payload, transport->fragment_length);
        message->type = WSS_TRANSPORT_MESSAGE_TEXT;
        message->payload_length = transport->fragment_length;
        message->close_code = 0U;
        release_fragment(transport);
        return WSS_TRANSPORT_RESULT_OK;
    }

    if (transport->fragment_payload != NULL) {
        close_connection(transport);
        return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
    }
    if (!header.final) {
        if (header.opcode != WSS_OPCODE_TEXT || frame_length > WSS_TRANSPORT_CONTROL_MAX_BYTES) {
            close_connection(transport);
            return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
        }
        transport->fragment_payload = malloc(WSS_TRANSPORT_CONTROL_MAX_BYTES);
        if (transport->fragment_payload == NULL) {
            close_connection(transport);
            return WSS_TRANSPORT_RESULT_NO_MEMORY;
        }
        result = buffered_read_exact(
            transport, transport->fragment_payload, frame_length, deadline);
        if (result != WSS_TRANSPORT_RESULT_OK) {
            close_connection(transport);
            return result;
        }
        transport->fragment_length = frame_length;
        transport->last_receive_time_ms = monotonic_ms();
        goto receive_next_frame;
    }

    const bool binary = header.opcode == WSS_OPCODE_BINARY;
    if ((binary && frame_length != WSS_TRANSPORT_PCM_MESSAGE_BYTES) ||
        (!binary && frame_length > WSS_TRANSPORT_CONTROL_MAX_BYTES) ||
        frame_length > payload_capacity) {
        close_connection(transport);
        return WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED;
    }
    result = buffered_read_exact(transport, payload, frame_length, deadline);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        close_connection(transport);
        return result;
    }
    if (!binary && !valid_utf8(payload, frame_length)) {
        close_connection(transport);
        return WSS_TRANSPORT_RESULT_PROTOCOL_ERROR;
    }
    transport->last_receive_time_ms = monotonic_ms();
    message->type = binary ? WSS_TRANSPORT_MESSAGE_BINARY : WSS_TRANSPORT_MESSAGE_TEXT;
    message->payload_length = frame_length;
    message->close_code = 0U;
    return WSS_TRANSPORT_RESULT_OK;
}

uint64_t wss_transport_last_receive_time_ms(const wss_transport_t *transport)
{
    return transport == NULL ? 0U : transport->last_receive_time_ms;
}
