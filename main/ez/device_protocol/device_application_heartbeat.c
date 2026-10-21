#include "device_application_heartbeat.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

#define DEVICE_APPLICATION_HEARTBEAT_MAX_SAFE_INTEGER 9007199254740991.0

typedef enum {
    HEARTBEAT_FIELD_NONCE = 0,
    HEARTBEAT_FIELD_SENT_AT_MS,
    HEARTBEAT_FIELD_COUNT,
} heartbeat_field_t;

static int heartbeat_field_index(const char *field_name)
{
    if (strcmp(field_name, "nonce") == 0) {
        return HEARTBEAT_FIELD_NONCE;
    }
    if (strcmp(field_name, "sent_at_ms") == 0) {
        return HEARTBEAT_FIELD_SENT_AT_MS;
    }
    return -1;
}

static bool payload_has_exact_fields(const cJSON *payload)
{
    bool present[HEARTBEAT_FIELD_COUNT] = {false};
    size_t field_count = 0U;
    for (const cJSON *field = payload->child; field != NULL; field = field->next) {
        if (field->string == NULL) {
            return false;
        }
        const int index = heartbeat_field_index(field->string);
        if (index < 0 || present[index]) {
            return false;
        }
        present[index] = true;
        ++field_count;
    }
    return field_count == HEARTBEAT_FIELD_COUNT && present[HEARTBEAT_FIELD_NONCE] &&
           present[HEARTBEAT_FIELD_SENT_AT_MS];
}

static bool nonce_is_valid(const char *nonce)
{
    if (nonce == NULL) {
        return false;
    }
    const size_t nonce_size = strlen(nonce);
    if (nonce_size == 0U || nonce_size > DEVICE_APPLICATION_HEARTBEAT_NONCE_MAX_BYTES) {
        return false;
    }
    for (size_t index = 0U; index < nonce_size; ++index) {
        if (isspace((unsigned char)nonce[index])) {
            return false;
        }
    }
    return true;
}

device_protocol_result_t
device_application_heartbeat_decode_ping(const device_control_envelope_t *envelope,
                                         device_application_heartbeat_t *heartbeat)
{
    if (envelope == NULL || heartbeat == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "ping") != 0) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    memset(heartbeat, 0, sizeof(*heartbeat));
    const cJSON *payload = device_control_envelope_payload(envelope);
    if (!cJSON_IsObject(payload) || !payload_has_exact_fields(payload)) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
    const cJSON *sent_at_ms = cJSON_GetObjectItemCaseSensitive(payload, "sent_at_ms");
    if (!cJSON_IsString(nonce) || !nonce_is_valid(nonce->valuestring) ||
        !cJSON_IsNumber(sent_at_ms) || !isfinite(sent_at_ms->valuedouble) ||
        sent_at_ms->valuedouble < 0.0 ||
        sent_at_ms->valuedouble > DEVICE_APPLICATION_HEARTBEAT_MAX_SAFE_INTEGER ||
        floor(sent_at_ms->valuedouble) != sent_at_ms->valuedouble) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    memcpy(heartbeat->nonce, nonce->valuestring, strlen(nonce->valuestring) + 1U);
    heartbeat->sent_at_ms = (int64_t)sent_at_ms->valuedouble;
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_application_heartbeat_encode_pong(
    const char *message_id, const device_application_heartbeat_t *heartbeat, char *encoded,
    size_t encoded_capacity, size_t *encoded_size)
{
    if (heartbeat == NULL || !nonce_is_valid(heartbeat->nonce) || heartbeat->sent_at_ms < 0 ||
        (double)heartbeat->sent_at_ms > DEVICE_APPLICATION_HEARTBEAT_MAX_SAFE_INTEGER) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddStringToObject(payload, "nonce", heartbeat->nonce) == NULL ||
        cJSON_AddNumberToObject(payload, "sent_at_ms", (double)heartbeat->sent_at_ms) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    const device_protocol_result_t result = device_control_envelope_encode(
        message_id, "pong", payload, encoded, encoded_capacity, encoded_size);
    cJSON_Delete(payload);
    return result;
}
