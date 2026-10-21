#ifndef HOST_DISCOVERY_H
#define HOST_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_protocol_version.h"

#define HOST_DISCOVERY_PATH_CAPACITY 16U
#define HOST_DISCOVERY_INSTALLATION_ID_LENGTH 32U
#define HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH 64U
#define HOST_DISCOVERY_MAX_RESULTS 16U

typedef enum {
    HOST_DISCOVERY_RESULT_OK = 0,
    HOST_DISCOVERY_RESULT_INVALID_ARGUMENT,
    HOST_DISCOVERY_RESULT_UNAVAILABLE,
    HOST_DISCOVERY_RESULT_NO_MEMORY,
    HOST_DISCOVERY_RESULT_CAPACITY_EXCEEDED,
} host_discovery_result_t;

typedef enum {
    HOST_SELECTION_SELECTED = 0,
    HOST_SELECTION_NOT_FOUND,
    HOST_SELECTION_MULTIPLE_PAIRING_HOSTS,
    HOST_SELECTION_CERTIFICATE_CHANGED,
    HOST_SELECTION_CONFLICT,
    HOST_SELECTION_INVALID_ARGUMENT,
} host_selection_result_t;

typedef struct {
    uint32_t ipv4_address;
    uint16_t port;
    uint16_t protocol_major;
    uint16_t protocol_minor;
    char path[HOST_DISCOVERY_PATH_CAPACITY];
    char installation_id[HOST_DISCOVERY_INSTALLATION_ID_LENGTH + 1U];
    char certificate_fingerprint[HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH + 1U];
    bool pairing_available;
} host_discovery_record_t;

typedef struct {
    bool paired;
    char installation_id[HOST_DISCOVERY_INSTALLATION_ID_LENGTH + 1U];
    char certificate_fingerprint[HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH + 1U];
} host_selection_identity_t;

host_discovery_result_t host_discovery_initialize(void);

void host_discovery_deinitialize(void);

host_discovery_result_t host_discovery_query(
    uint32_t timeout_ms,
    host_discovery_record_t *records,
    size_t record_capacity,
    size_t *record_count);

bool host_discovery_record_is_compatible(const host_discovery_record_t *record);

host_selection_result_t host_discovery_select(
    const host_discovery_record_t *records,
    size_t record_count,
    const host_selection_identity_t *identity,
    host_discovery_record_t *selected_record);

#endif
