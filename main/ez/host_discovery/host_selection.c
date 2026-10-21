#include "host_discovery.h"

#include <string.h>

static bool is_hex_string(const char *value, size_t expected_length)
{
    if (value == NULL || value[expected_length] != '\0') {
        return false;
    }
    for (size_t index = 0; index < expected_length; ++index) {
        const char character = value[index];
        const bool decimal = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!decimal && !lower && !upper) {
            return false;
        }
    }
    return true;
}

static bool path_is_device(const char path[HOST_DISCOVERY_PATH_CAPACITY])
{
    static const char expected[] = "/device";
    return sizeof(expected) <= HOST_DISCOVERY_PATH_CAPACITY &&
           memcmp(path, expected, sizeof(expected)) == 0;
}

static bool same_host_identity(
    const host_discovery_record_t *left,
    const host_discovery_record_t *right)
{
    return strcmp(left->installation_id, right->installation_id) == 0 &&
           strcmp(left->certificate_fingerprint, right->certificate_fingerprint) == 0;
}

static bool endpoint_precedes(
    const host_discovery_record_t *candidate,
    const host_discovery_record_t *current)
{
    if (candidate->ipv4_address != current->ipv4_address) {
        return candidate->ipv4_address < current->ipv4_address;
    }
    return candidate->port < current->port;
}

bool host_discovery_record_is_compatible(const host_discovery_record_t *record)
{
    return record != NULL && record->ipv4_address != 0U && record->port != 0U &&
           record->protocol_major == DEVICE_PROTOCOL_MAJOR &&
           path_is_device(record->path) &&
           is_hex_string(record->installation_id, HOST_DISCOVERY_INSTALLATION_ID_LENGTH) &&
           is_hex_string(
               record->certificate_fingerprint,
               HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH);
}

host_selection_result_t host_discovery_select(
    const host_discovery_record_t *records,
    size_t record_count,
    const host_selection_identity_t *identity,
    host_discovery_record_t *selected_record)
{
    if ((records == NULL && record_count != 0U) || identity == NULL || selected_record == NULL) {
        return HOST_SELECTION_INVALID_ARGUMENT;
    }
    if (identity->paired &&
        (!is_hex_string(identity->installation_id, HOST_DISCOVERY_INSTALLATION_ID_LENGTH) ||
         !is_hex_string(
             identity->certificate_fingerprint,
             HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH))) {
        return HOST_SELECTION_INVALID_ARGUMENT;
    }

    const host_discovery_record_t *selection = NULL;
    bool certificate_changed = false;
    for (size_t index = 0; index < record_count; ++index) {
        const host_discovery_record_t *record = &records[index];
        if (!host_discovery_record_is_compatible(record)) {
            continue;
        }

        if (identity->paired) {
            if (strcmp(record->installation_id, identity->installation_id) != 0) {
                continue;
            }
            if (strcmp(
                    record->certificate_fingerprint,
                    identity->certificate_fingerprint) != 0) {
                certificate_changed = true;
                continue;
            }
        } else if (!record->pairing_available) {
            continue;
        }

        if (selection == NULL) {
            selection = record;
            continue;
        }
        if (!same_host_identity(selection, record)) {
            return identity->paired ? HOST_SELECTION_CONFLICT
                                    : HOST_SELECTION_MULTIPLE_PAIRING_HOSTS;
        }
        if (endpoint_precedes(record, selection)) {
            selection = record;
        }
    }

    if (selection != NULL) {
        *selected_record = *selection;
        return HOST_SELECTION_SELECTED;
    }
    if (identity->paired && certificate_changed) {
        return HOST_SELECTION_CERTIFICATE_CHANGED;
    }
    return HOST_SELECTION_NOT_FOUND;
}
