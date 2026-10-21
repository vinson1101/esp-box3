#include "device_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "device_identity_record.h"
#include "nvs.h"

#ifndef DEVICE_IDENTITY_NVS_NAMESPACE
#define DEVICE_IDENTITY_NVS_NAMESPACE "dev_identity"
#endif
#define DEVICE_IDENTITY_NVS_KEY "credential"

struct device_identity {
    nvs_handle_t nvs_handle;
};

static void secure_zero(void *memory, size_t memory_size)
{
    volatile uint8_t *bytes = memory;
    while (memory_size > 0U) {
        *bytes++ = 0U;
        --memory_size;
    }
}

static device_identity_result_t map_nvs_error(esp_err_t error)
{
    if (error == ESP_OK) {
        return DEVICE_IDENTITY_RESULT_OK;
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return DEVICE_IDENTITY_RESULT_NOT_FOUND;
    }
    if (error == ESP_ERR_NO_MEM) {
        return DEVICE_IDENTITY_RESULT_NO_MEMORY;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return DEVICE_IDENTITY_RESULT_CORRUPT;
    }
    return DEVICE_IDENTITY_RESULT_STORAGE_FAILURE;
}

static device_identity_result_t persist_record(
    device_identity_t *identity,
    const device_identity_record_t *record)
{
    uint8_t encoded[DEVICE_IDENTITY_RECORD_ENCODED_BYTES];
    size_t encoded_size = 0U;
    device_identity_result_t result = device_identity_record_encode(
        record,
        encoded,
        sizeof(encoded),
        &encoded_size);
    if (result != DEVICE_IDENTITY_RESULT_OK) {
        secure_zero(encoded, sizeof(encoded));
        return result;
    }
    esp_err_t nvs_result = nvs_set_blob(
        identity->nvs_handle,
        DEVICE_IDENTITY_NVS_KEY,
        encoded,
        encoded_size);
    secure_zero(encoded, sizeof(encoded));
    if (nvs_result != ESP_OK) {
        return map_nvs_error(nvs_result);
    }
    nvs_result = nvs_commit(identity->nvs_handle);
    return map_nvs_error(nvs_result);
}

device_identity_result_t device_identity_open(device_identity_t **identity)
{
    if (identity == NULL || *identity != NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    device_identity_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return DEVICE_IDENTITY_RESULT_NO_MEMORY;
    }
    const esp_err_t result = nvs_open(
        DEVICE_IDENTITY_NVS_NAMESPACE,
        NVS_READWRITE,
        &created->nvs_handle);
    if (result != ESP_OK) {
        free(created);
        return map_nvs_error(result);
    }
    *identity = created;
    return DEVICE_IDENTITY_RESULT_OK;
}

device_identity_result_t device_identity_load(
    device_identity_t *identity,
    device_identity_record_t *record)
{
    if (identity == NULL || record == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    uint8_t encoded[DEVICE_IDENTITY_RECORD_ENCODED_BYTES];
    size_t encoded_size = sizeof(encoded);
    const esp_err_t result = nvs_get_blob(
        identity->nvs_handle,
        DEVICE_IDENTITY_NVS_KEY,
        encoded,
        &encoded_size);
    if (result != ESP_OK) {
        secure_zero(encoded, sizeof(encoded));
        return map_nvs_error(result);
    }
    const device_identity_result_t decode_result =
        device_identity_record_decode(encoded, encoded_size, record);
    secure_zero(encoded, sizeof(encoded));
    return decode_result;
}

device_identity_result_t device_identity_save_pending(
    device_identity_t *identity,
    const device_identity_record_t *record)
{
    if (identity == NULL || record == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    if (record->state != DEVICE_IDENTITY_STATE_PENDING) {
        return DEVICE_IDENTITY_RESULT_INVALID_STATE;
    }
    return persist_record(identity, record);
}

device_identity_result_t device_identity_activate(
    device_identity_t *identity,
    const char *expected_device_id)
{
    if (identity == NULL || expected_device_id == NULL || expected_device_id[0] == '\0') {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    device_identity_record_t record = {0};
    device_identity_result_t result = device_identity_load(identity, &record);
    if (result != DEVICE_IDENTITY_RESULT_OK) {
        secure_zero(&record, sizeof(record));
        return result;
    }
    if (record.state != DEVICE_IDENTITY_STATE_PENDING ||
        strcmp(record.device_id, expected_device_id) != 0) {
        secure_zero(&record, sizeof(record));
        return DEVICE_IDENTITY_RESULT_INVALID_STATE;
    }
    record.state = DEVICE_IDENTITY_STATE_ACTIVE;
    result = persist_record(identity, &record);
    secure_zero(&record, sizeof(record));
    return result;
}

device_identity_result_t device_identity_clear_credential(
    device_identity_t *identity,
    bool retain_host_binding)
{
    if (identity == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    if (!retain_host_binding) {
        return device_identity_clear_all(identity);
    }
    device_identity_record_t record = {0};
    device_identity_result_t result = device_identity_load(identity, &record);
    if (result == DEVICE_IDENTITY_RESULT_NOT_FOUND) {
        return DEVICE_IDENTITY_RESULT_OK;
    }
    if (result != DEVICE_IDENTITY_RESULT_OK) {
        secure_zero(&record, sizeof(record));
        return result;
    }
    record.state = DEVICE_IDENTITY_STATE_NONE;
    memset(record.device_id, 0, sizeof(record.device_id));
    secure_zero(record.private_seed, sizeof(record.private_seed));
    secure_zero(record.public_key, sizeof(record.public_key));
    result = persist_record(identity, &record);
    secure_zero(&record, sizeof(record));
    return result;
}

device_identity_result_t device_identity_clear_all(device_identity_t *identity)
{
    if (identity == NULL) {
        return DEVICE_IDENTITY_RESULT_INVALID_ARGUMENT;
    }
    esp_err_t result = nvs_erase_key(identity->nvs_handle, DEVICE_IDENTITY_NVS_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return DEVICE_IDENTITY_RESULT_OK;
    }
    if (result != ESP_OK) {
        return map_nvs_error(result);
    }
    result = nvs_commit(identity->nvs_handle);
    return map_nvs_error(result);
}

void device_identity_close(device_identity_t *identity)
{
    if (identity == NULL) {
        return;
    }
    nvs_close(identity->nvs_handle);
    secure_zero(identity, sizeof(*identity));
    free(identity);
}
