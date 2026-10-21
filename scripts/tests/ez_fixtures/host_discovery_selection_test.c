#include "host_discovery.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const INSTALLATION_A = "0123456789abcdef0123456789abcdef";
static const char *const INSTALLATION_B = "fedcba9876543210fedcba9876543210";
static const char *const FINGERPRINT_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *const FINGERPRINT_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static host_discovery_record_t record(
    const char *installation_id,
    const char *fingerprint,
    uint32_t address,
    bool pairing_available)
{
    host_discovery_record_t value = {
        .ipv4_address = address,
        .port = 43125U,
        .protocol_major = DEVICE_PROTOCOL_MAJOR,
        .protocol_minor = DEVICE_PROTOCOL_MINOR,
        .path = "/device",
        .pairing_available = pairing_available,
    };
    assert(strlen(installation_id) < sizeof(value.installation_id));
    assert(strlen(fingerprint) < sizeof(value.certificate_fingerprint));
    strcpy(value.installation_id, installation_id);
    strcpy(value.certificate_fingerprint, fingerprint);
    return value;
}

int main(void)
{
    host_discovery_record_t records[] = {
        record(INSTALLATION_A, FINGERPRINT_A, 0x0200000aU, false),
        record(INSTALLATION_B, FINGERPRINT_B, 0x0300000aU, true),
    };
    host_discovery_record_t selected;
    host_selection_identity_t unpaired = {0};

    assert(host_discovery_select(records, 2U, &unpaired, &selected) == HOST_SELECTION_SELECTED);
    assert(strcmp(selected.installation_id, INSTALLATION_B) == 0);

    records[0].pairing_available = true;
    assert(host_discovery_select(records, 2U, &unpaired, &selected) ==
           HOST_SELECTION_MULTIPLE_PAIRING_HOSTS);

    records[1].pairing_available = false;
    host_selection_identity_t paired = {.paired = true};
    strcpy(paired.installation_id, INSTALLATION_A);
    strcpy(paired.certificate_fingerprint, FINGERPRINT_A);
    assert(host_discovery_select(records, 2U, &paired, &selected) == HOST_SELECTION_SELECTED);
    assert(strcmp(selected.installation_id, INSTALLATION_A) == 0);

    strcpy(records[0].certificate_fingerprint, FINGERPRINT_B);
    assert(host_discovery_select(records, 2U, &paired, &selected) ==
           HOST_SELECTION_CERTIFICATE_CHANGED);

    records[0] = record(INSTALLATION_A, FINGERPRINT_A, 0x0400000aU, true);
    records[1] = record(INSTALLATION_A, FINGERPRINT_A, 0x0100000aU, true);
    assert(host_discovery_select(records, 2U, &unpaired, &selected) == HOST_SELECTION_SELECTED);
    assert(selected.ipv4_address == 0x0100000aU);

    records[0].protocol_major = DEVICE_PROTOCOL_MAJOR + 1U;
    assert(!host_discovery_record_is_compatible(&records[0]));
    records[1].pairing_available = false;
    assert(host_discovery_select(records, 2U, &unpaired, &selected) ==
           HOST_SELECTION_NOT_FOUND);

    puts("host discovery selection tests passed");
    return 0;
}
