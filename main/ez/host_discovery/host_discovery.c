#include "host_discovery.h"
#include "legacy_discovery.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "mdns.h"

#define HOST_DISCOVERY_SERVICE_TYPE "_ez-assistant"
#define HOST_DISCOVERY_SERVICE_PROTOCOL "_tcp"
#define HOST_DISCOVERY_COMPLETION_GRACE_MS 250U

static mdns_search_once_t *active_query;
static bool service_initialized;

static host_discovery_result_t copy_text(
    char *destination,
    size_t destination_capacity,
    const char *source,
    size_t source_length)
{
    if (destination == NULL || destination_capacity == 0U || source == NULL ||
        source_length >= destination_capacity) {
        return HOST_DISCOVERY_RESULT_INVALID_ARGUMENT;
    }
    memcpy(destination, source, source_length);
    destination[source_length] = '\0';
    return HOST_DISCOVERY_RESULT_OK;
}

static bool text_value(
    const mdns_result_t *result,
    const char *key,
    const char **value,
    size_t *value_length)
{
    for (size_t index = 0; index < result->txt_count; ++index) {
        if (result->txt[index].key != NULL && strcmp(result->txt[index].key, key) == 0 &&
            result->txt[index].value != NULL) {
            *value = result->txt[index].value;
            *value_length = result->txt_value_len == NULL
                                ? strlen(result->txt[index].value)
                                : result->txt_value_len[index];
            return true;
        }
    }
    return false;
}

static bool parse_uint16(const char *value, size_t value_length, uint16_t *parsed)
{
    if (value == NULL || parsed == NULL || value_length == 0U || value_length > 5U) {
        return false;
    }
    uint32_t accumulated = 0U;
    for (size_t index = 0; index < value_length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        accumulated = accumulated * 10U + (uint32_t)(value[index] - '0');
        if (accumulated > UINT16_MAX) {
            return false;
        }
    }
    *parsed = (uint16_t)accumulated;
    return true;
}

static bool parse_boolean(const char *value, size_t value_length, bool *parsed)
{
    if (value_length == 4U && memcmp(value, "true", 4U) == 0) {
        *parsed = true;
        return true;
    }
    if (value_length == 5U && memcmp(value, "false", 5U) == 0) {
        *parsed = false;
        return true;
    }
    return false;
}

static bool first_ipv4(const mdns_result_t *result, uint32_t *address)
{
    for (const mdns_ip_addr_t *item = result->addr; item != NULL; item = item->next) {
        if (item->addr.type == ESP_IPADDR_TYPE_V4 && item->addr.u_addr.ip4.addr != 0U) {
            *address = item->addr.u_addr.ip4.addr;
            return true;
        }
    }
    return false;
}

static bool parse_record(const mdns_result_t *result, host_discovery_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->port = result->port;
    if (!first_ipv4(result, &record->ipv4_address)) {
        return false;
    }

    const char *value = NULL;
    size_t value_length = 0U;
    if (!text_value(result, "protocol_major", &value, &value_length) ||
        !parse_uint16(value, value_length, &record->protocol_major) ||
        !text_value(result, "protocol_minor", &value, &value_length) ||
        !parse_uint16(value, value_length, &record->protocol_minor) ||
        !text_value(result, "path", &value, &value_length) ||
        copy_text(record->path, sizeof(record->path), value, value_length) !=
            HOST_DISCOVERY_RESULT_OK ||
        !text_value(result, "installation_id", &value, &value_length) ||
        copy_text(record->installation_id, sizeof(record->installation_id), value, value_length) !=
            HOST_DISCOVERY_RESULT_OK ||
        !text_value(result, "certificate_fingerprint", &value, &value_length) ||
        copy_text(
            record->certificate_fingerprint,
            sizeof(record->certificate_fingerprint),
            value,
            value_length) != HOST_DISCOVERY_RESULT_OK ||
        !text_value(result, "pairing_available", &value, &value_length) ||
        !parse_boolean(value, value_length, &record->pairing_available)) {
        return false;
    }
    return host_discovery_record_is_compatible(record);
}

host_discovery_result_t host_discovery_initialize(void)
{
    if (service_initialized) {
        return HOST_DISCOVERY_RESULT_OK;
    }
    const esp_err_t result = mdns_init();
    if (result == ESP_OK) {
        active_query = NULL;
        service_initialized = true;
        return HOST_DISCOVERY_RESULT_OK;
    }
    return result == ESP_ERR_NO_MEM ? HOST_DISCOVERY_RESULT_NO_MEMORY
                                    : HOST_DISCOVERY_RESULT_UNAVAILABLE;
}

void host_discovery_deinitialize(void)
{
    if (!service_initialized) {
        return;
    }
    active_query = NULL;
    mdns_free();
    service_initialized = false;
}

static host_discovery_result_t restart_discovery_service(void)
{
    active_query = NULL;
    mdns_free();
    service_initialized = false;

    const esp_err_t result = mdns_init();
    if (result == ESP_OK) {
        service_initialized = true;
        return HOST_DISCOVERY_RESULT_UNAVAILABLE;
    }
    return result == ESP_ERR_NO_MEM ? HOST_DISCOVERY_RESULT_NO_MEMORY
                                    : HOST_DISCOVERY_RESULT_UNAVAILABLE;
}

static host_discovery_result_t query_results(
    uint32_t timeout_ms,
    size_t max_results,
    mdns_result_t **results)
{
    mdns_search_once_t *query = active_query;
    uint8_t result_count = 0U;
    if (query != NULL) {
        if (!mdns_query_async_get_results(query, 0U, results, &result_count)) {
            return restart_discovery_service();
        }
    } else {
        query = mdns_query_async_new(
            NULL,
            HOST_DISCOVERY_SERVICE_TYPE,
            HOST_DISCOVERY_SERVICE_PROTOCOL,
            MDNS_TYPE_PTR,
            timeout_ms,
            max_results,
            NULL);
        if (query == NULL) {
            return HOST_DISCOVERY_RESULT_NO_MEMORY;
        }
        active_query = query;
        if (!mdns_query_async_get_results(
                query,
                timeout_ms + HOST_DISCOVERY_COMPLETION_GRACE_MS,
                results,
                &result_count)) {
            return HOST_DISCOVERY_RESULT_UNAVAILABLE;
        }
    }
    if (mdns_query_async_delete(query) != ESP_OK) {
        *results = NULL;
        return restart_discovery_service();
    }
    active_query = NULL;
    return HOST_DISCOVERY_RESULT_OK;
}

host_discovery_result_t host_discovery_query(
    uint32_t timeout_ms,
    host_discovery_record_t *records,
    size_t record_capacity,
    size_t *record_count)
{
    if (timeout_ms == 0U ||
        timeout_ms > UINT32_MAX - HOST_DISCOVERY_COMPLETION_GRACE_MS || records == NULL ||
        record_capacity == 0U || record_count == NULL ||
        record_capacity > HOST_DISCOVERY_MAX_RESULTS) {
        return HOST_DISCOVERY_RESULT_INVALID_ARGUMENT;
    }
    *record_count = 0U;

    if (!service_initialized) {
        const host_discovery_result_t initialization_result = host_discovery_initialize();
        if (initialization_result != HOST_DISCOVERY_RESULT_OK) {
            return initialization_result;
        }
    }

    mdns_result_t *results = NULL;
    const host_discovery_result_t query_result = query_results(
        timeout_ms,
        record_capacity + 1U,
        &results);
    if (query_result != HOST_DISCOVERY_RESULT_OK) {
        return legacy_discovery_query(1500U, records, record_capacity, record_count);
    }

    host_discovery_result_t status = HOST_DISCOVERY_RESULT_OK;
    size_t raw_count = 0U;
    for (const mdns_result_t *item = results; item != NULL; item = item->next) {
        ++raw_count;
        host_discovery_record_t record;
        if (!parse_record(item, &record)) {
            ESP_LOGW("EZDiscovery", "Incomplete record: ipv4=%08lx port=%u txt=%u protocol=%u.%u",
                     (unsigned long)record.ipv4_address, (unsigned)record.port,
                     (unsigned)item->txt_count, (unsigned)record.protocol_major,
                     (unsigned)record.protocol_minor);
            continue;
        }
        if (*record_count == record_capacity) {
            status = HOST_DISCOVERY_RESULT_CAPACITY_EXCEEDED;
            break;
        }
        records[*record_count] = record;
        ++(*record_count);
    }
    ESP_LOGI("EZDiscovery", "Query raw=%u usable=%u status=%d", (unsigned)raw_count,
             (unsigned)*record_count, status);
    mdns_query_results_free(results);
    if (status == HOST_DISCOVERY_RESULT_OK && *record_count == 0U) {
        return legacy_discovery_query(1500U, records, record_capacity, record_count);
    }
    return status;
}
