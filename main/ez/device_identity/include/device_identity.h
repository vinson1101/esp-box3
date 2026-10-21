#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_ed25519.h"
#include "host_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_IDENTITY_DEVICE_ID_MAX_BYTES 256U

typedef struct device_identity device_identity_t;

typedef enum {
    DEVICE_IDENTITY_RESULT_OK = 0,
    DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT,
    DEVICE_IDENTITY_RESULT_NO_MEMORY,
    DEVICE_IDENTITY_RESULT_NOT_FOUND,
    DEVICE_IDENTITY_RESULT_INVALID_STATE,
    DEVICE_IDENTITY_RESULT_CORRUPT,
    DEVICE_IDENTITY_RESULT_STORAGE_FAILURE,
} device_identity_result_t;

typedef enum {
    DEVICE_IDENTITY_STATE_NONE = 0,
    DEVICE_IDENTITY_STATE_PENDING,
    DEVICE_IDENTITY_STATE_ACTIVE,
} device_identity_state_t;

typedef struct {
    device_identity_state_t state;
    char device_id[DEVICE_IDENTITY_DEVICE_ID_MAX_BYTES + 1U];
    char host_installation_id[HOST_DISCOVERY_INSTALLATION_ID_LENGTH + 1U];
    char host_certificate_fingerprint[
        HOST_DISCOVERY_CERTIFICATE_FINGERPRINT_LENGTH + 1U];
    uint8_t private_seed[DEVICE_ED25519_SEED_BYTES];
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES];
} device_identity_record_t;

device_identity_result_t device_identity_record_validate(
    const device_identity_record_t *record);

device_identity_result_t device_identity_open(device_identity_t **identity);

device_identity_result_t device_identity_load(
    device_identity_t *identity,
    device_identity_record_t *record);

device_identity_result_t device_identity_save_pending(
    device_identity_t *identity,
    const device_identity_record_t *record);

device_identity_result_t device_identity_activate(
    device_identity_t *identity,
    const char *expected_device_id);

device_identity_result_t device_identity_clear_credential(
    device_identity_t *identity,
    bool retain_host_binding);

device_identity_result_t device_identity_clear_all(device_identity_t *identity);

void device_identity_close(device_identity_t *identity);

#ifdef __cplusplus
}
#endif
