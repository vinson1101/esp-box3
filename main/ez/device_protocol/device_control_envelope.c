#include "device_control_envelope.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct device_control_envelope {
    cJSON *document;
    uint16_t protocol_minor;
    const char *message_id;
    const char *message_type;
    const cJSON *payload;
};

typedef enum {
    ENVELOPE_FIELD_PROTOCOL_MAJOR = 0,
    ENVELOPE_FIELD_PROTOCOL_MINOR,
    ENVELOPE_FIELD_MESSAGE_ID,
    ENVELOPE_FIELD_MESSAGE_TYPE,
    ENVELOPE_FIELD_PAYLOAD,
    ENVELOPE_FIELD_COUNT,
} envelope_field_t;

static bool string_has_visible_content(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t value_size = strlen(value);
    if (value_size == 0U || value_size > maximum_size) {
        return false;
    }
    for (size_t index = 0U; index < value_size; ++index) {
        if (!isspace((unsigned char)value[index])) {
            return true;
        }
    }
    return false;
}

static bool number_is_u16(const cJSON *value, uint16_t *parsed)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0.0
        || value->valuedouble > (double)UINT16_MAX
        || floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *parsed = (uint16_t)value->valuedouble;
    return true;
}

static int envelope_field_index(const char *field_name)
{
    if (strcmp(field_name, "protocol_major") == 0) {
        return ENVELOPE_FIELD_PROTOCOL_MAJOR;
    }
    if (strcmp(field_name, "protocol_minor") == 0) {
        return ENVELOPE_FIELD_PROTOCOL_MINOR;
    }
    if (strcmp(field_name, "message_id") == 0) {
        return ENVELOPE_FIELD_MESSAGE_ID;
    }
    if (strcmp(field_name, "type") == 0) {
        return ENVELOPE_FIELD_MESSAGE_TYPE;
    }
    if (strcmp(field_name, "payload") == 0) {
        return ENVELOPE_FIELD_PAYLOAD;
    }
    return -1;
}

static bool envelope_has_exact_fields(const cJSON *document)
{
    bool present[ENVELOPE_FIELD_COUNT] = {false};
    size_t field_count = 0U;
    for (const cJSON *field = document->child; field != NULL; field = field->next) {
        if (field->string == NULL) {
            return false;
        }
        const int index = envelope_field_index(field->string);
        if (index < 0 || present[index]) {
            return false;
        }
        present[index] = true;
        field_count += 1U;
    }
    if (field_count != ENVELOPE_FIELD_COUNT) {
        return false;
    }
    for (size_t index = 0U; index < ENVELOPE_FIELD_COUNT; ++index) {
        if (!present[index]) {
            return false;
        }
    }
    return true;
}

static bool trailing_bytes_are_whitespace(const char *parse_end, const char *encoded_end)
{
    for (const char *cursor = parse_end; cursor < encoded_end; ++cursor) {
        if (!isspace((unsigned char)*cursor)) {
            return false;
        }
    }
    return true;
}

static bool contains_escaped_nul(const char* encoded, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (encoded[i] != '\\')
            continue;
        size_t count = 1;
        while (i + count < size && encoded[i + count] == '\\')
            ++count;
        i += count;
        if ((count & 1U) && i + 5 <= size && memcmp(encoded + i, "u0000", 5) == 0)
            return true;
    }
    return false;
}

device_protocol_result_t device_control_envelope_decode(
    const char *encoded,
    size_t encoded_size,
    device_control_envelope_t **envelope)
{
    if (encoded == NULL || envelope == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    *envelope = NULL;
    if (encoded_size > DEVICE_CONTROL_MESSAGE_MAX_BYTES) {
        return DEVICE_PROTOCOL_RESULT_MESSAGE_TOO_LARGE;
    }
    // cJSON cannot preserve embedded NULs; reject rather than truncate text/IDs.
    if (contains_escaped_nul(encoded, encoded_size))
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;

    const char *parse_end = NULL;
    cJSON *document = cJSON_ParseWithLengthOpts(encoded, encoded_size, &parse_end, false);
    if (document == NULL || parse_end == NULL
        || !trailing_bytes_are_whitespace(parse_end, encoded + encoded_size)) {
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_INVALID_JSON;
    }
    if (!cJSON_IsObject(document) || !envelope_has_exact_fields(document)) {
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }

    const cJSON *protocol_major = cJSON_GetObjectItemCaseSensitive(document, "protocol_major");
    const cJSON *protocol_minor = cJSON_GetObjectItemCaseSensitive(document, "protocol_minor");
    const cJSON *message_id = cJSON_GetObjectItemCaseSensitive(document, "message_id");
    const cJSON *message_type = cJSON_GetObjectItemCaseSensitive(document, "type");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(document, "payload");
    uint16_t parsed_major = 0U;
    uint16_t parsed_minor = 0U;
    if (!number_is_u16(protocol_major, &parsed_major) || !number_is_u16(protocol_minor, &parsed_minor)
        || !cJSON_IsString(message_id) || !cJSON_IsString(message_type)
        || !string_has_visible_content(message_id->valuestring, DEVICE_MESSAGE_ID_MAX_BYTES)
        || !string_has_visible_content(message_type->valuestring, DEVICE_MESSAGE_TYPE_MAX_BYTES)
        || payload == NULL) {
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE;
    }
    if (parsed_major != DEVICE_PROTOCOL_MAJOR) {
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_UNSUPPORTED_PROTOCOL;
    }

    device_control_envelope_t *decoded = calloc(1U, sizeof(*decoded));
    if (decoded == NULL) {
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    decoded->document = document;
    decoded->protocol_minor = parsed_minor;
    decoded->message_id = message_id->valuestring;
    decoded->message_type = message_type->valuestring;
    decoded->payload = payload;
    *envelope = decoded;
    return DEVICE_PROTOCOL_RESULT_OK;
}

device_protocol_result_t device_control_envelope_encode(
    const char *message_id,
    const char *message_type,
    const cJSON *payload,
    char *encoded,
    size_t encoded_capacity,
    size_t *encoded_size)
{
    if (payload == NULL || encoded_size == NULL
        || !string_has_visible_content(message_id, DEVICE_MESSAGE_ID_MAX_BYTES)
        || !string_has_visible_content(message_type, DEVICE_MESSAGE_TYPE_MAX_BYTES)) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }

    cJSON *document = cJSON_CreateObject();
    cJSON *payload_copy = cJSON_Duplicate(payload, true);
    if (document == NULL || payload_copy == NULL
        || cJSON_AddNumberToObject(document, "protocol_major", DEVICE_PROTOCOL_MAJOR) == NULL
        || cJSON_AddNumberToObject(document, "protocol_minor", DEVICE_PROTOCOL_MINOR) == NULL
        || cJSON_AddStringToObject(document, "message_id", message_id) == NULL
        || cJSON_AddStringToObject(document, "type", message_type) == NULL
        || !cJSON_AddItemToObject(document, "payload", payload_copy)) {
        cJSON_Delete(payload_copy);
        cJSON_Delete(document);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    payload_copy = NULL;

    char *serialized = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (serialized == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    const size_t serialized_size = strlen(serialized);
    *encoded_size = serialized_size;
    if (serialized_size > DEVICE_CONTROL_MESSAGE_MAX_BYTES) {
        cJSON_free(serialized);
        return DEVICE_PROTOCOL_RESULT_MESSAGE_TOO_LARGE;
    }
    if (serialized_size > encoded_capacity || (serialized_size > 0U && encoded == NULL)) {
        cJSON_free(serialized);
        return DEVICE_PROTOCOL_RESULT_OUTPUT_TOO_SMALL;
    }
    memcpy(encoded, serialized, serialized_size);
    cJSON_free(serialized);
    return DEVICE_PROTOCOL_RESULT_OK;
}

uint16_t device_control_envelope_protocol_minor(const device_control_envelope_t *envelope)
{
    return envelope == NULL ? 0U : envelope->protocol_minor;
}

const char *device_control_envelope_message_id(const device_control_envelope_t *envelope)
{
    return envelope == NULL ? NULL : envelope->message_id;
}

const char *device_control_envelope_message_type(const device_control_envelope_t *envelope)
{
    return envelope == NULL ? NULL : envelope->message_type;
}

const cJSON *device_control_envelope_payload(const device_control_envelope_t *envelope)
{
    return envelope == NULL ? NULL : envelope->payload;
}

void device_control_envelope_release(device_control_envelope_t *envelope)
{
    if (envelope == NULL) {
        return;
    }
    cJSON_Delete(envelope->document);
    free(envelope);
}
