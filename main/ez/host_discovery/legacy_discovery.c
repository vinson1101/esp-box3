#include "legacy_discovery.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#define LEGACY_PACKET_CAPACITY 4096U

typedef struct {
    host_discovery_record_t records[HOST_DISCOVERY_MAX_RESULTS];
    uint8_t packet[LEGACY_PACKET_CAPACITY + 1U];
} legacy_buffer_t;

host_discovery_result_t legacy_discovery_query(uint32_t timeout_ms,
                                               host_discovery_record_t* records, size_t capacity,
                                               size_t* count) {
    // RFC 6762 sections 5.1/6.7: an ephemeral source port requests a direct reply.
    static const uint8_t query[] = {
        0,   0,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   13, '_',
        'e', 'z', '-', 'a', 's', 's', 'i', 's', 't', 'a', 'n', 't', 4,  '_',
        't', 'c', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0,   0,   12,  0,  1,
    };
    if (records == NULL || count == NULL || timeout_ms == 0U || capacity == 0U ||
        capacity > HOST_DISCOVERY_MAX_RESULTS)
        return HOST_DISCOVERY_RESULT_INVALID_ARGUMENT;
    *count = 0U;
    legacy_buffer_t* buffer = malloc(sizeof(*buffer));
    if (buffer == NULL)
        return HOST_DISCOVERY_RESULT_NO_MEMORY;
    int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    host_discovery_result_t status = HOST_DISCOVERY_RESULT_UNAVAILABLE;
    if (descriptor < 0)
        goto done;
    struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    const struct sockaddr_in destination = {.sin_family = AF_INET,
                                            .sin_port = htons(5353),
                                            .sin_addr.s_addr = inet_addr("224.0.0.251")};
    const int ttl = 255;
    if (bind(descriptor, (const struct sockaddr*)&local, sizeof(local)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0)
        goto done;
    const int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    int64_t next_send = 0;
    unsigned datagrams = 0U;
    status = HOST_DISCOVERY_RESULT_OK;
    while (esp_timer_get_time() / 1000 < deadline && datagrams < 64U) {
        const int64_t now = esp_timer_get_time() / 1000;
        if (now >= next_send) {
            if (sendto(descriptor, query, sizeof(query), 0, (const struct sockaddr*)&destination,
                       sizeof(destination)) != (int)sizeof(query)) {
                status = HOST_DISCOVERY_RESULT_UNAVAILABLE;
                break;
            }
            next_send = now + 500;
        }
        const int64_t remaining = deadline - now;
        const int64_t wait_ms = remaining < 100 ? remaining : 100;
        if (wait_ms <= 0)
            break;
        const struct timeval timeout = {.tv_sec = 0, .tv_usec = wait_ms * 1000};
        if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            status = HOST_DISCOVERY_RESULT_UNAVAILABLE;
            break;
        }
        struct sockaddr_in source = {0};
        socklen_t source_length = sizeof(source);
        const int received = recvfrom(descriptor, buffer->packet, sizeof(buffer->packet), 0,
                                      (struct sockaddr*)&source, &source_length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            status = HOST_DISCOVERY_RESULT_UNAVAILABLE;
            break;
        }
        ++datagrams;
        if (received > (int)LEGACY_PACKET_CAPACITY || source.sin_family != AF_INET ||
            source.sin_port != htons(5353))
            continue;
        size_t found = 0U;
        const host_discovery_result_t parsed = legacy_discovery_parse(
            buffer->packet, received, source.sin_addr.s_addr, buffer->records, capacity, &found);
        if (parsed == HOST_DISCOVERY_RESULT_CAPACITY_EXCEEDED) {
            status = parsed;
            break;
        }
        if (parsed != HOST_DISCOVERY_RESULT_OK)
            continue;
        for (size_t index = 0U; index < found; ++index) {
            bool duplicate = false;
            for (size_t prior = 0U; prior < *count; ++prior) {
                const host_discovery_record_t *old = &records[prior],
                                              *item = &buffer->records[index];
                if (old->ipv4_address == item->ipv4_address && old->port == item->port &&
                    strcmp(old->installation_id, item->installation_id) == 0 &&
                    strcmp(old->certificate_fingerprint, item->certificate_fingerprint) == 0)
                    duplicate = true;
            }
            if (duplicate)
                continue;
            if (*count == capacity) {
                status = HOST_DISCOVERY_RESULT_CAPACITY_EXCEEDED;
                goto done;
            }
            records[(*count)++] = buffer->records[index];
        }
    }
    if (datagrams == 64U)
        status = HOST_DISCOVERY_RESULT_UNAVAILABLE;
done:
    if (descriptor >= 0)
        close(descriptor);
    free(buffer);
    ESP_LOGI("EZDiscovery", "Legacy unicast query usable=%u status=%d", (unsigned)*count, status);
    return status;
}
