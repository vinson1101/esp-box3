#include "legacy_discovery.h"

#include <string.h>

#define SERVICE_NAME "_ez-assistant._tcp.local."
#define DNS_NAME_CAPACITY 256U
#define DNS_RECORD_LIMIT 64U

typedef struct {
    char name[DNS_NAME_CAPACITY];
    uint16_t type;
    size_t data;
    size_t end;
    bool live;
} dns_record_t;

static uint16_t read_u16(const uint8_t* p) { return ((uint16_t)p[0] << 8U) | p[1]; }

static bool read_name(const uint8_t* packet, size_t length, size_t* position, char* name) {
    size_t cursor = *position, written = 0U;
    bool jumped = false;
    for (size_t step = 0U; step < 128U && cursor < length; ++step) {
        const size_t start = cursor;
        const uint8_t label = packet[cursor++];
        if ((label & 0xc0U) == 0xc0U) {
            if (cursor >= length)
                return false;
            const size_t target = ((size_t)(label & 0x3fU) << 8U) | packet[cursor++];
            if (target >= start)
                return false;
            if (!jumped)
                *position = cursor;
            jumped = true;
            cursor = target;
        } else if (label == 0U) {
            if (!jumped)
                *position = cursor;
            name[written] = '\0';
            return true;
        } else {
            if (label > 63U || label > length - cursor || written + label + 1U >= DNS_NAME_CAPACITY)
                return false;
            for (size_t index = 0U; index < label; ++index) {
                uint8_t character = packet[cursor++];
                if (character == 0U || character == '.')
                    return false;
                if (character >= 'A' && character <= 'Z')
                    character += 'a' - 'A';
                name[written++] = (char)character;
            }
            name[written++] = '.';
        }
    }
    return false;
}

static bool read_record(const uint8_t* packet, size_t length, size_t* position,
                        dns_record_t* record) {
    if (!read_name(packet, length, position, record->name) || length - *position < 10U)
        return false;
    const uint8_t* header = packet + *position;
    record->type = read_u16(header);
    record->live = (read_u16(header + 2U) & 0x7fffU) == 1U &&
                   (header[4] | header[5] | header[6] | header[7]) != 0U;
    record->data = *position + 10U;
    const size_t size = read_u16(header + 8U);
    if (size > length - record->data)
        return false;
    record->end = record->data + size;
    *position = record->end;
    return true;
}

static bool number(const uint8_t* value, size_t length, uint16_t* result) {
    if (length == 0U || length > 5U)
        return false;
    uint32_t parsed = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return false;
        parsed = parsed * 10U + value[index] - '0';
    }
    if (parsed > UINT16_MAX)
        return false;
    *result = (uint16_t)parsed;
    return true;
}

static bool read_txt(const uint8_t* packet, const dns_record_t* rr,
                     host_discovery_record_t* record) {
    static const char* keys[] = {"protocol_major",  "protocol_minor",          "path",
                                 "installation_id", "certificate_fingerprint", "pairing_available"};
    unsigned found = 0U;
    for (size_t position = rr->data; position < rr->end;) {
        const size_t size = packet[position++];
        if (size > rr->end - position)
            return false;
        const uint8_t* text = packet + position;
        position += size;
        const uint8_t* equals = memchr(text, '=', size);
        if (equals == NULL)
            continue;
        const size_t key_length = (size_t)(equals - text), value_length = size - key_length - 1U;
        const uint8_t* value = equals + 1U;
        for (size_t key = 0U; key < sizeof(keys) / sizeof(keys[0]); ++key) {
            if (strlen(keys[key]) != key_length || memcmp(text, keys[key], key_length) != 0)
                continue;
            if ((found & (1U << key)) != 0U || memchr(value, 0, value_length) != NULL)
                return false;
            found |= 1U << key;
            if (key < 2U) {
                if (!number(value, value_length,
                            key == 0U ? &record->protocol_major : &record->protocol_minor))
                    return false;
            } else if (key == 5U) {
                if (value_length == 4U && memcmp(value, "true", 4U) == 0)
                    record->pairing_available = true;
                else if (value_length == 5U && memcmp(value, "false", 5U) == 0)
                    record->pairing_available = false;
                else
                    return false;
            } else {
                char* destination = key == 2U   ? record->path
                                    : key == 3U ? record->installation_id
                                                : record->certificate_fingerprint;
                const size_t capacity = key == 2U   ? sizeof(record->path)
                                        : key == 3U ? sizeof(record->installation_id)
                                                    : sizeof(record->certificate_fingerprint);
                if (value_length >= capacity)
                    return false;
                memcpy(destination, value, value_length);
                destination[value_length] = '\0';
            }
        }
    }
    return found == 0x3fU;
}

static bool resolve_instance(const uint8_t* packet, size_t length, size_t start, size_t total,
                             const char* instance, uint32_t source_address,
                             host_discovery_record_t* record) {
    char hostname[DNS_NAME_CAPACITY] = {0};
    bool srv = false, txt = false;
    memset(record, 0, sizeof(*record));
    size_t position = start;
    for (size_t index = 0U; index < total; ++index) {
        dns_record_t rr;
        if (!read_record(packet, length, &position, &rr))
            return false;
        if (!rr.live || strcmp(rr.name, instance) != 0)
            continue;
        if (rr.type == 33U) {
            if (srv || rr.end - rr.data < 7U)
                return false;
            srv = true;
            record->port = read_u16(packet + rr.data + 4U);
            size_t target = rr.data + 6U;
            if (!read_name(packet, length, &target, hostname) || target != rr.end)
                return false;
        } else if (rr.type == 16U) {
            if (txt || !read_txt(packet, &rr, record))
                return false;
            txt = true;
        }
    }
    if (!srv || !txt)
        return false;
    position = start;
    for (size_t index = 0U; index < total; ++index) {
        dns_record_t rr;
        if (!read_record(packet, length, &position, &rr))
            return false;
        if (rr.live && rr.type == 1U && rr.end - rr.data == 4U && strcmp(rr.name, hostname) == 0) {
            uint32_t address;
            memcpy(&address, packet + rr.data, sizeof(address));
            // Use the responder's actual interface, never its loopback or VPN address.
            if (address == source_address)
                record->ipv4_address = address;
        }
    }
    return host_discovery_record_is_compatible(record);
}

host_discovery_result_t legacy_discovery_parse(const uint8_t* packet, size_t length,
                                               uint32_t source_address,
                                               host_discovery_record_t* records, size_t capacity,
                                               size_t* count) {
    if (packet == NULL || records == NULL || count == NULL || capacity == 0U)
        return HOST_DISCOVERY_RESULT_INVALID_ARGUMENT;
    *count = 0U;
    // The one-shot query uses ID 0; its fresh UDP socket provides request isolation.
    if (length < 12U || read_u16(packet) != 0U || (read_u16(packet + 2U) & 0xfa0fU) != 0x8000U ||
        read_u16(packet + 4U) != 1U)
        return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    size_t position = 12U;
    char name[DNS_NAME_CAPACITY];
    if (!read_name(packet, length, &position, name) || strcmp(name, SERVICE_NAME) != 0 ||
        length - position < 4U || read_u16(packet + position) != 12U ||
        read_u16(packet + position + 2U) != 1U)
        return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    position += 4U;
    const size_t start = position;
    const size_t total =
        (size_t)read_u16(packet + 6U) + read_u16(packet + 8U) + read_u16(packet + 10U);
    if (total > DNS_RECORD_LIMIT)
        return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    // Validate the whole datagram before publishing any result.
    for (size_t index = 0U; index < total; ++index) {
        dns_record_t rr;
        if (!read_record(packet, length, &position, &rr))
            return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    }
    if (position != length)
        return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    position = start;
    for (size_t index = 0U; index < total; ++index) {
        dns_record_t rr;
        if (!read_record(packet, length, &position, &rr))
            return HOST_DISCOVERY_RESULT_UNAVAILABLE;
        if (!rr.live || rr.type != 12U || strcmp(rr.name, SERVICE_NAME) != 0)
            continue;
        size_t alias_position = rr.data;
        if (!read_name(packet, length, &alias_position, name) || alias_position != rr.end)
            continue;
        host_discovery_record_t record;
        if (!resolve_instance(packet, length, start, total, name, source_address, &record))
            continue;
        if (*count == capacity)
            return HOST_DISCOVERY_RESULT_CAPACITY_EXCEEDED;
        records[(*count)++] = record;
    }
    return HOST_DISCOVERY_RESULT_OK;
}
