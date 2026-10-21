#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_control_envelope.h"
#include "device_identity.h"
#include "device_pairing_protocol.h"
#include "device_random_source.h"
#include "host_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PAIRING_CODE_DIGITS 6U
#define DEVICE_PAIRING_CODE_CAPACITY (DEVICE_PAIRING_CODE_DIGITS + 1U)
#define DEVICE_PAIRING_MAX_SECURITY_FAILURES 5U
#define DEVICE_PAIRING_MAX_OUTBOUND_MESSAGES 2U

typedef struct device_pairing device_pairing_t;

typedef enum {
    DEVICE_PAIRING_RESULT_OK = 0,
    DEVICE_PAIRING_RESULT_INVALID_ARGUMENT,
    DEVICE_PAIRING_RESULT_INVALID_STATE,
    DEVICE_PAIRING_RESULT_NO_MEMORY,
    DEVICE_PAIRING_RESULT_OUTPUT_TOO_SMALL,
    DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED,
    DEVICE_PAIRING_RESULT_PROTOCOL_ERROR,
    DEVICE_PAIRING_RESULT_STORAGE_ERROR,
} device_pairing_result_t;

typedef enum {
    DEVICE_PAIRING_STATE_READY = 0,
    DEVICE_PAIRING_STATE_HELLO_SENT,
    DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL,
    DEVICE_PAIRING_STATE_WAITING_FOR_BIND_ACK,
    DEVICE_PAIRING_STATE_WAITING_FOR_COMPLETE,
    DEVICE_PAIRING_STATE_COMPLETE,
} device_pairing_state_t;

typedef struct {
    device_identity_t *identity_store;
    device_capabilities_t capabilities;
    const char *display_name;
    device_random_source_t random_source;
} device_pairing_config_t;

typedef struct {
    size_t message_count;
    size_t message_sizes[DEVICE_PAIRING_MAX_OUTBOUND_MESSAGES];
    bool pending_credential_saved;
    bool identity_activated;
    bool attempt_reset;
    bool pairing_code_rotated;
} device_pairing_transition_t;

device_pairing_result_t device_pairing_create(
    const device_pairing_config_t *config,
    device_pairing_t **pairing);

device_pairing_result_t device_pairing_copy_code(
    const device_pairing_t *pairing,
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY]);

device_pairing_state_t device_pairing_state(const device_pairing_t *pairing);

uint8_t device_pairing_security_failure_count(const device_pairing_t *pairing);

device_pairing_result_t device_pairing_begin(
    device_pairing_t *pairing,
    const host_discovery_record_t *host,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_pairing_result_t device_pairing_handle_control(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    char *first_encoded,
    size_t first_capacity,
    char *second_encoded,
    size_t second_capacity,
    device_pairing_transition_t *transition);

device_pairing_result_t device_pairing_connection_lost(
    device_pairing_t *pairing,
    bool pairing_attempt_still_valid,
    device_pairing_transition_t *transition);

device_pairing_result_t device_pairing_force_new_code(device_pairing_t *pairing);

device_pairing_result_t device_pairing_copy_identity(
    const device_pairing_t *pairing,
    device_identity_record_t *identity);

void device_pairing_destroy(device_pairing_t *pairing);

#ifdef __cplusplus
}
#endif
