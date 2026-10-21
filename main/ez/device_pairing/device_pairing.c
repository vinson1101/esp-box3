#include "device_pairing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_base64url.h"
#include "device_ed25519.h"
#include "device_spake2.h"
#include "device_transcript.h"

#define PAIRING_RANDOM_TOKEN_BYTES 18U
#define PAIRING_RANDOM_TOKEN_CHARACTERS 24U
#define PAIRING_DEVICE_NONCE_BYTES 32U
#define PAIRING_DEVICE_NONCE_CHARACTERS 43U
#define PAIRING_TRANSCRIPT_CAPACITY 4096U
#define PAIRING_CODE_RANGE 1000000U

struct device_pairing {
    device_identity_t *identity_store;
    device_capabilities_t capabilities;
    device_random_source_t random_source;
    device_pairing_state_t state;
    uint8_t security_failure_count;
    char display_name[DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES + 1U];
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY];
    host_discovery_record_t host;
    char pairing_request_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U];
    char hello_message_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U];
    char device_nonce[PAIRING_DEVICE_NONCE_CHARACTERS + 1U];
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES];
    device_spake2_party_a_t *pake_session;
    device_spake2_keys_t *keys;
    device_identity_record_t provisional_identity;
    uint8_t associated_data[PAIRING_TRANSCRIPT_CAPACITY];
    size_t associated_data_size;
    uint8_t bind_transcript[PAIRING_TRANSCRIPT_CAPACITY];
    size_t bind_transcript_size;
};

static void secure_zero(void *memory, size_t memory_size)
{
    volatile uint8_t *bytes = memory;
    while (memory_size > 0U) {
        *bytes++ = 0U;
        --memory_size;
    }
}

static bool nonempty_bounded(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, maximum_size + 1U);
    return length > 0U && length <= maximum_size;
}

static bool random_fill(device_pairing_t *pairing, uint8_t *output, size_t output_size)
{
    return pairing->random_source.fill != NULL &&
           pairing->random_source.fill(
               pairing->random_source.context,
               output,
               output_size);
}

static bool random_token(
    device_pairing_t *pairing,
    size_t random_size,
    char *encoded,
    size_t encoded_capacity)
{
    uint8_t random_bytes[PAIRING_DEVICE_NONCE_BYTES];
    if (random_size > sizeof(random_bytes) || !random_fill(pairing, random_bytes, random_size)) {
        secure_zero(random_bytes, sizeof(random_bytes));
        return false;
    }
    size_t encoded_size = 0U;
    const bool encoded_ok = device_base64url_encode(
                                random_bytes,
                                random_size,
                                encoded,
                                encoded_capacity - 1U,
                                &encoded_size) == DEVICE_CRYPTO_RESULT_OK &&
                            encoded_size < encoded_capacity;
    secure_zero(random_bytes, sizeof(random_bytes));
    if (!encoded_ok) {
        return false;
    }
    encoded[encoded_size] = '\0';
    return true;
}

static bool generate_pairing_code(device_pairing_t *pairing)
{
    const uint32_t acceptance_limit = UINT32_MAX - (UINT32_MAX % PAIRING_CODE_RANGE);
    uint32_t value = acceptance_limit;
    for (size_t attempt = 0U; attempt < 128U && value >= acceptance_limit; ++attempt) {
        uint8_t random_bytes[4U];
        if (!random_fill(pairing, random_bytes, sizeof(random_bytes))) {
            secure_zero(random_bytes, sizeof(random_bytes));
            return false;
        }
        value = ((uint32_t)random_bytes[0] << 24U) |
                ((uint32_t)random_bytes[1] << 16U) |
                ((uint32_t)random_bytes[2] << 8U) |
                (uint32_t)random_bytes[3];
        secure_zero(random_bytes, sizeof(random_bytes));
    }
    if (value >= acceptance_limit) {
        return false;
    }
    value %= PAIRING_CODE_RANGE;
    for (size_t index = DEVICE_PAIRING_CODE_DIGITS; index > 0U; --index) {
        pairing->pairing_code[index - 1U] = (char)('0' + value % 10U);
        value /= 10U;
    }
    pairing->pairing_code[DEVICE_PAIRING_CODE_DIGITS] = '\0';
    return true;
}

static void attempt_secrets_release(device_pairing_t *pairing)
{
    device_spake2_party_a_release(pairing->pake_session);
    pairing->pake_session = NULL;
    device_spake2_keys_release(pairing->keys);
    pairing->keys = NULL;
    secure_zero(pairing->pairing_request_id, sizeof(pairing->pairing_request_id));
    secure_zero(pairing->hello_message_id, sizeof(pairing->hello_message_id));
    secure_zero(pairing->device_nonce, sizeof(pairing->device_nonce));
    secure_zero(pairing->device_share, sizeof(pairing->device_share));
    secure_zero(&pairing->host, sizeof(pairing->host));
    secure_zero(&pairing->provisional_identity, sizeof(pairing->provisional_identity));
    secure_zero(pairing->associated_data, sizeof(pairing->associated_data));
    pairing->associated_data_size = 0U;
    secure_zero(pairing->bind_transcript, sizeof(pairing->bind_transcript));
    pairing->bind_transcript_size = 0U;
    pairing->state = DEVICE_PAIRING_STATE_READY;
}

static device_pairing_result_t attempt_create(
    device_pairing_t *pairing,
    const host_discovery_record_t *host)
{
    attempt_secrets_release(pairing);
    pairing->host = *host;
    if (!random_token(
            pairing,
            PAIRING_RANDOM_TOKEN_BYTES,
            pairing->pairing_request_id,
            sizeof(pairing->pairing_request_id)) ||
        !random_token(
            pairing,
            PAIRING_RANDOM_TOKEN_BYTES,
            pairing->hello_message_id,
            sizeof(pairing->hello_message_id)) ||
        !random_token(
            pairing,
            PAIRING_DEVICE_NONCE_BYTES,
            pairing->device_nonce,
            sizeof(pairing->device_nonce)) ||
        device_spake2_party_a_begin(
            pairing->pairing_code,
            pairing->pairing_request_id,
            host->installation_id,
            &pairing->random_source,
            &pairing->pake_session,
            pairing->device_share) != DEVICE_CRYPTO_RESULT_OK) {
        attempt_secrets_release(pairing);
        return DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
    }
    return DEVICE_PAIRING_RESULT_OK;
}

static bool same_host(
    const host_discovery_record_t *left,
    const host_discovery_record_t *right)
{
    return strcmp(left->installation_id, right->installation_id) == 0 &&
           strcmp(left->certificate_fingerprint, right->certificate_fingerprint) == 0;
}

static device_pairing_result_t create_message_id(
    device_pairing_t *pairing,
    char message_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U])
{
    return random_token(
               pairing,
               PAIRING_RANDOM_TOKEN_BYTES,
               message_id,
               PAIRING_RANDOM_TOKEN_CHARACTERS + 1U)
        ? DEVICE_PAIRING_RESULT_OK
        : DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
}

static device_pairing_result_t protocol_result(
    device_pairing_protocol_result_t result)
{
    switch (result) {
        case DEVICE_PAIRING_PROTOCOL_RESULT_OK:
            return DEVICE_PAIRING_RESULT_OK;
        case DEVICE_PAIRING_PROTOCOL_RESULT_OUTPUT_TOO_SMALL:
            return DEVICE_PAIRING_RESULT_OUTPUT_TOO_SMALL;
        case DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_ARGUMENT:
            return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
        case DEVICE_PAIRING_PROTOCOL_RESULT_CRYPTOGRAPHY_FAILED:
            return DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
        default:
            return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
    }
}

device_pairing_result_t device_pairing_create(
    const device_pairing_config_t *config,
    device_pairing_t **pairing)
{
    if (config == NULL || pairing == NULL || *pairing != NULL ||
        config->identity_store == NULL || config->random_source.fill == NULL ||
        !nonempty_bounded(config->display_name, DEVICE_PAIRING_DISPLAY_NAME_MAX_BYTES)) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    device_pairing_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return DEVICE_PAIRING_RESULT_NO_MEMORY;
    }
    created->identity_store = config->identity_store;
    created->capabilities = config->capabilities;
    created->random_source = config->random_source;
    memcpy(
        created->display_name,
        config->display_name,
        strlen(config->display_name) + 1U);
    if (!generate_pairing_code(created)) {
        device_pairing_destroy(created);
        return DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
    }
    *pairing = created;
    return DEVICE_PAIRING_RESULT_OK;
}

device_pairing_result_t device_pairing_copy_code(
    const device_pairing_t *pairing,
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY])
{
    if (pairing == NULL || pairing_code == NULL) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    memcpy(pairing_code, pairing->pairing_code, sizeof(pairing->pairing_code));
    return DEVICE_PAIRING_RESULT_OK;
}

device_pairing_state_t device_pairing_state(const device_pairing_t *pairing)
{
    return pairing == NULL ? DEVICE_PAIRING_STATE_READY : pairing->state;
}

uint8_t device_pairing_security_failure_count(const device_pairing_t *pairing)
{
    return pairing == NULL ? 0U : pairing->security_failure_count;
}

device_pairing_result_t device_pairing_begin(
    device_pairing_t *pairing,
    const host_discovery_record_t *host,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (pairing == NULL || host == NULL || encoded == NULL || encoded_size == NULL ||
        !host_discovery_record_is_compatible(host) || !host->pairing_available) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    const bool resumable = (pairing->state == DEVICE_PAIRING_STATE_HELLO_SENT ||
                            pairing->state == DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL) &&
                           same_host(&pairing->host, host) && pairing->pake_session != NULL;
    if (!resumable) {
        if (pairing->state != DEVICE_PAIRING_STATE_READY) {
            return DEVICE_PAIRING_RESULT_INVALID_STATE;
        }
        const device_pairing_result_t create_result = attempt_create(pairing, host);
        if (create_result != DEVICE_PAIRING_RESULT_OK) {
            return create_result;
        }
    }
    const device_pairing_result_t result = protocol_result(device_pairing_encode_hello(
        pairing->hello_message_id,
        pairing->pairing_request_id,
        pairing->display_name,
        pairing->device_nonce,
        &pairing->capabilities,
        pairing->device_share,
        encoded,
        encoded_capacity,
        encoded_size));
    if (result == DEVICE_PAIRING_RESULT_OK) {
        pairing->state = DEVICE_PAIRING_STATE_HELLO_SENT;
    }
    return result;
}

static device_pairing_result_t security_failure(
    device_pairing_t *pairing,
    device_pairing_transition_t *transition)
{
    if (pairing->security_failure_count < UINT8_MAX) {
        ++pairing->security_failure_count;
    }
    attempt_secrets_release(pairing);
    transition->attempt_reset = true;
    if (pairing->security_failure_count >= DEVICE_PAIRING_MAX_SECURITY_FAILURES) {
        if (!generate_pairing_code(pairing)) {
            return DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
        }
        pairing->security_failure_count = 0U;
        transition->pairing_code_rotated = true;
    }
    return DEVICE_PAIRING_RESULT_OK;
}

static device_pairing_result_t handle_pending(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope)
{
    if (pairing->state != DEVICE_PAIRING_STATE_HELLO_SENT) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    device_pairing_pending_t pending = {0};
    const device_pairing_result_t result = protocol_result(device_pairing_decode_pending(
        envelope,
        pairing->pairing_request_id,
        &pending));
    if (result == DEVICE_PAIRING_RESULT_OK) {
        pairing->state = DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL;
    }
    return result;
}

static device_pairing_result_t handle_pake(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    char *first_encoded,
    size_t first_capacity,
    char *second_encoded,
    size_t second_capacity,
    device_pairing_transition_t *transition)
{
    if (pairing->state != DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL ||
        pairing->pake_session == NULL) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    device_pairing_pake_t pake = {0};
    device_pairing_result_t result = protocol_result(device_pairing_decode_pake(
        envelope,
        pairing->pairing_request_id,
        &pake));
    if (result != DEVICE_PAIRING_RESULT_OK) {
        return result;
    }
    if (device_pairing_associated_data_encode(
            pairing->pairing_request_id,
            pairing->host.installation_id,
            pairing->host.certificate_fingerprint,
            pairing->device_nonce,
            pake.host_nonce,
            &pairing->capabilities,
            pairing->associated_data,
            sizeof(pairing->associated_data),
            &pairing->associated_data_size) != DEVICE_CRYPTO_RESULT_OK ||
        device_spake2_party_a_finish(
            pairing->pake_session,
            pake.host_share,
            pairing->associated_data,
            pairing->associated_data_size,
            &pairing->keys) != DEVICE_CRYPTO_RESULT_OK) {
        secure_zero(&pake, sizeof(pake));
        return security_failure(pairing, transition);
    }
    device_spake2_party_a_release(pairing->pake_session);
    pairing->pake_session = NULL;
    if (device_spake2_keys_verify_host_confirmation(
            pairing->keys,
            pake.host_confirmation) != DEVICE_CRYPTO_RESULT_OK) {
        secure_zero(&pake, sizeof(pake));
        return security_failure(pairing, transition);
    }
    secure_zero(&pake, sizeof(pake));

    uint8_t device_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES];
    uint8_t bind_signature[DEVICE_ED25519_SIGNATURE_BYTES];
    uint8_t bind_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES];
    char first_message_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U];
    char second_message_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U];
    if (device_spake2_keys_copy_device_confirmation(
            pairing->keys,
            device_confirmation) != DEVICE_CRYPTO_RESULT_OK ||
        !random_fill(
            pairing,
            pairing->provisional_identity.private_seed,
            sizeof(pairing->provisional_identity.private_seed)) ||
        device_ed25519_derive_public_key(
            pairing->provisional_identity.private_seed,
            pairing->provisional_identity.public_key) != DEVICE_CRYPTO_RESULT_OK ||
        device_pairing_bind_transcript_encode(
            pairing->associated_data,
            pairing->associated_data_size,
            pairing->provisional_identity.public_key,
            pairing->bind_transcript,
            sizeof(pairing->bind_transcript),
            &pairing->bind_transcript_size) != DEVICE_CRYPTO_RESULT_OK ||
        device_ed25519_sign(
            pairing->provisional_identity.private_seed,
            pairing->bind_transcript,
            pairing->bind_transcript_size,
            bind_signature) != DEVICE_CRYPTO_RESULT_OK ||
        device_spake2_keys_binding_mac(
            pairing->keys,
            (const uint8_t *)"device-bind",
            strlen("device-bind"),
            pairing->bind_transcript,
            pairing->bind_transcript_size,
            bind_mac) != DEVICE_CRYPTO_RESULT_OK ||
        create_message_id(pairing, first_message_id) != DEVICE_PAIRING_RESULT_OK ||
        create_message_id(pairing, second_message_id) != DEVICE_PAIRING_RESULT_OK) {
        secure_zero(device_confirmation, sizeof(device_confirmation));
        secure_zero(bind_signature, sizeof(bind_signature));
        secure_zero(bind_mac, sizeof(bind_mac));
        return security_failure(pairing, transition);
    }
    result = protocol_result(device_pairing_encode_confirmation(
        first_message_id,
        pairing->pairing_request_id,
        device_confirmation,
        first_encoded,
        first_capacity,
        &transition->message_sizes[0]));
    if (result == DEVICE_PAIRING_RESULT_OK) {
        result = protocol_result(device_pairing_encode_bind(
            second_message_id,
            pairing->pairing_request_id,
            pairing->provisional_identity.public_key,
            bind_signature,
            bind_mac,
            second_encoded,
            second_capacity,
            &transition->message_sizes[1]));
    }
    secure_zero(device_confirmation, sizeof(device_confirmation));
    secure_zero(bind_signature, sizeof(bind_signature));
    secure_zero(bind_mac, sizeof(bind_mac));
    secure_zero(first_message_id, sizeof(first_message_id));
    secure_zero(second_message_id, sizeof(second_message_id));
    if (result == DEVICE_PAIRING_RESULT_OK) {
        transition->message_count = 2U;
        pairing->state = DEVICE_PAIRING_STATE_WAITING_FOR_BIND_ACK;
    } else {
        attempt_secrets_release(pairing);
        transition->attempt_reset = true;
    }
    return result;
}

static device_pairing_result_t handle_bind_ack(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    char *encoded,
    size_t encoded_capacity,
    device_pairing_transition_t *transition)
{
    if (pairing->state != DEVICE_PAIRING_STATE_WAITING_FOR_BIND_ACK ||
        pairing->keys == NULL) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    device_pairing_bind_ack_t ack = {0};
    device_pairing_result_t result = protocol_result(device_pairing_decode_bind_ack(
        envelope,
        pairing->pairing_request_id,
        &ack));
    if (result != DEVICE_PAIRING_RESULT_OK) {
        return result;
    }
    uint8_t *commit_transcript = malloc(PAIRING_TRANSCRIPT_CAPACITY);
    if (commit_transcript == NULL) {
        secure_zero(&ack, sizeof(ack));
        return DEVICE_PAIRING_RESULT_NO_MEMORY;
    }
    size_t commit_transcript_size = 0U;
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES];
    uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES];
    char message_id[PAIRING_RANDOM_TOKEN_CHARACTERS + 1U];
    if (device_pairing_commit_transcript_encode(
            pairing->bind_transcript,
            pairing->bind_transcript_size,
            ack.device_id,
            commit_transcript,
            PAIRING_TRANSCRIPT_CAPACITY,
            &commit_transcript_size) != DEVICE_CRYPTO_RESULT_OK ||
        device_spake2_keys_verify_binding_mac(
            pairing->keys,
            (const uint8_t *)"host-bind-ack",
            strlen("host-bind-ack"),
            commit_transcript,
            commit_transcript_size,
            ack.host_proof) != DEVICE_CRYPTO_RESULT_OK) {
        secure_zero(commit_transcript, PAIRING_TRANSCRIPT_CAPACITY);
        free(commit_transcript);
        secure_zero(&ack, sizeof(ack));
        return security_failure(pairing, transition);
    }
    pairing->provisional_identity.state = DEVICE_IDENTITY_STATE_PENDING;
    memcpy(
        pairing->provisional_identity.device_id,
        ack.device_id,
        strlen(ack.device_id) + 1U);
    memcpy(
        pairing->provisional_identity.host_installation_id,
        pairing->host.installation_id,
        sizeof(pairing->provisional_identity.host_installation_id));
    memcpy(
        pairing->provisional_identity.host_certificate_fingerprint,
        pairing->host.certificate_fingerprint,
        sizeof(pairing->provisional_identity.host_certificate_fingerprint));
    if (device_identity_save_pending(
            pairing->identity_store,
            &pairing->provisional_identity) != DEVICE_IDENTITY_RESULT_OK) {
        secure_zero(commit_transcript, PAIRING_TRANSCRIPT_CAPACITY);
        free(commit_transcript);
        secure_zero(&ack, sizeof(ack));
        return DEVICE_PAIRING_RESULT_STORAGE_ERROR;
    }
    transition->pending_credential_saved = true;
    pairing->state = DEVICE_PAIRING_STATE_WAITING_FOR_COMPLETE;
    if (device_ed25519_sign(
            pairing->provisional_identity.private_seed,
            commit_transcript,
            commit_transcript_size,
            signature) != DEVICE_CRYPTO_RESULT_OK ||
        device_spake2_keys_binding_mac(
            pairing->keys,
            (const uint8_t *)"device-commit",
            strlen("device-commit"),
            commit_transcript,
            commit_transcript_size,
            binding_mac) != DEVICE_CRYPTO_RESULT_OK ||
        create_message_id(pairing, message_id) != DEVICE_PAIRING_RESULT_OK) {
        result = DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
    } else {
        result = protocol_result(device_pairing_encode_commit(
            message_id,
            pairing->pairing_request_id,
            ack.device_id,
            signature,
            binding_mac,
            encoded,
            encoded_capacity,
            &transition->message_sizes[0]));
    }
    secure_zero(commit_transcript, PAIRING_TRANSCRIPT_CAPACITY);
    free(commit_transcript);
    secure_zero(signature, sizeof(signature));
    secure_zero(binding_mac, sizeof(binding_mac));
    secure_zero(message_id, sizeof(message_id));
    secure_zero(&ack, sizeof(ack));
    device_spake2_keys_release(pairing->keys);
    pairing->keys = NULL;
    if (result == DEVICE_PAIRING_RESULT_OK) {
        transition->message_count = 1U;
    }
    return result;
}

static device_pairing_result_t handle_complete(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    device_pairing_transition_t *transition)
{
    if (pairing->state != DEVICE_PAIRING_STATE_WAITING_FOR_COMPLETE) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    device_pairing_complete_t complete = {0};
    const device_pairing_result_t result = protocol_result(device_pairing_decode_complete(
        envelope,
        pairing->pairing_request_id,
        pairing->provisional_identity.device_id,
        &complete));
    secure_zero(&complete, sizeof(complete));
    if (result != DEVICE_PAIRING_RESULT_OK) {
        return result;
    }
    if (device_identity_activate(
            pairing->identity_store,
            pairing->provisional_identity.device_id) != DEVICE_IDENTITY_RESULT_OK) {
        return DEVICE_PAIRING_RESULT_STORAGE_ERROR;
    }
    pairing->provisional_identity.state = DEVICE_IDENTITY_STATE_ACTIVE;
    pairing->state = DEVICE_PAIRING_STATE_COMPLETE;
    transition->identity_activated = true;
    pairing->security_failure_count = 0U;
    return DEVICE_PAIRING_RESULT_OK;
}

static device_pairing_result_t handle_error(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    device_pairing_transition_t *transition)
{
    static const char *const expected_fields[] = {
        "code", "correlation_message_id", "recoverable"};
    const cJSON *payload = device_control_envelope_payload(envelope);
    bool fields_seen[3U] = {false};
    size_t field_count = 0U;
    if (!cJSON_IsObject(payload)) {
        return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
    }
    for (const cJSON *field = payload->child; field != NULL; field = field->next) {
        if (field->string == NULL || field_count >= 3U) {
            return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
        }
        size_t matched = 3U;
        for (size_t index = 0U; index < 3U; ++index) {
            if (strcmp(field->string, expected_fields[index]) == 0) {
                matched = index;
                break;
            }
        }
        if (matched == 3U || fields_seen[matched]) {
            return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
        }
        fields_seen[matched] = true;
        ++field_count;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(payload, "code");
    const cJSON *correlation =
        cJSON_GetObjectItemCaseSensitive(payload, "correlation_message_id");
    const cJSON *recoverable = cJSON_GetObjectItemCaseSensitive(payload, "recoverable");
    if (field_count != 3U || !fields_seen[0] || !fields_seen[1] || !fields_seen[2] ||
        !cJSON_IsString(code) || !nonempty_bounded(code->valuestring, 128U) ||
        (!cJSON_IsNull(correlation) &&
         (!cJSON_IsString(correlation) ||
          !nonempty_bounded(correlation->valuestring, DEVICE_MESSAGE_ID_MAX_BYTES))) ||
        !cJSON_IsBool(recoverable) || !cJSON_IsTrue(recoverable)) {
        return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
    }
    if (pairing->state == DEVICE_PAIRING_STATE_READY ||
        pairing->state == DEVICE_PAIRING_STATE_COMPLETE) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    if (strcmp(code->valuestring, "pairing_failed") == 0) {
        return security_failure(pairing, transition);
    }
    if (strcmp(code->valuestring, "pairing_not_open") == 0) {
        attempt_secrets_release(pairing);
        transition->attempt_reset = true;
        return DEVICE_PAIRING_RESULT_OK;
    }
    return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
}

device_pairing_result_t device_pairing_handle_control(
    device_pairing_t *pairing,
    const device_control_envelope_t *envelope,
    char *first_encoded,
    size_t first_capacity,
    char *second_encoded,
    size_t second_capacity,
    device_pairing_transition_t *transition)
{
    if (pairing == NULL || envelope == NULL || transition == NULL || first_encoded == NULL ||
        first_capacity == 0U || second_encoded == NULL || second_capacity == 0U) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    memset(transition, 0, sizeof(*transition));
    const char *message_type = device_control_envelope_message_type(envelope);
    if (strcmp(message_type, "pairing_pending") == 0) {
        return handle_pending(pairing, envelope);
    }
    if (strcmp(message_type, "pairing_pake") == 0) {
        return handle_pake(
            pairing,
            envelope,
            first_encoded,
            first_capacity,
            second_encoded,
            second_capacity,
            transition);
    }
    if (strcmp(message_type, "pairing_bind_ack") == 0) {
        return handle_bind_ack(
            pairing,
            envelope,
            first_encoded,
            first_capacity,
            transition);
    }
    if (strcmp(message_type, "pairing_complete") == 0) {
        return handle_complete(pairing, envelope, transition);
    }
    if (strcmp(message_type, "error") == 0) {
        return handle_error(pairing, envelope, transition);
    }
    return DEVICE_PAIRING_RESULT_PROTOCOL_ERROR;
}

device_pairing_result_t device_pairing_connection_lost(
    device_pairing_t *pairing,
    bool pairing_attempt_still_valid,
    device_pairing_transition_t *transition)
{
    if (pairing == NULL || transition == NULL) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    memset(transition, 0, sizeof(*transition));
    if (pairing_attempt_still_valid &&
        (pairing->state == DEVICE_PAIRING_STATE_HELLO_SENT ||
         pairing->state == DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL) &&
        pairing->pake_session != NULL) {
        return DEVICE_PAIRING_RESULT_OK;
    }
    if (pairing->state != DEVICE_PAIRING_STATE_COMPLETE) {
        attempt_secrets_release(pairing);
        transition->attempt_reset = true;
    }
    return DEVICE_PAIRING_RESULT_OK;
}

device_pairing_result_t device_pairing_force_new_code(device_pairing_t *pairing)
{
    if (pairing == NULL) {
        return DEVICE_PAIRING_RESULT_INVALID_ARGUMENT;
    }
    attempt_secrets_release(pairing);
    pairing->security_failure_count = 0U;
    return generate_pairing_code(pairing) ? DEVICE_PAIRING_RESULT_OK
                                          : DEVICE_PAIRING_RESULT_CRYPTOGRAPHY_FAILED;
}

device_pairing_result_t device_pairing_copy_identity(
    const device_pairing_t *pairing,
    device_identity_record_t *identity)
{
    if (pairing == NULL || identity == NULL ||
        (pairing->state != DEVICE_PAIRING_STATE_WAITING_FOR_COMPLETE &&
         pairing->state != DEVICE_PAIRING_STATE_COMPLETE)) {
        return DEVICE_PAIRING_RESULT_INVALID_STATE;
    }
    *identity = pairing->provisional_identity;
    return DEVICE_PAIRING_RESULT_OK;
}

void device_pairing_destroy(device_pairing_t *pairing)
{
    if (pairing == NULL) {
        return;
    }
    attempt_secrets_release(pairing);
    secure_zero(pairing->pairing_code, sizeof(pairing->pairing_code));
    secure_zero(pairing, sizeof(*pairing));
    free(pairing);
}
