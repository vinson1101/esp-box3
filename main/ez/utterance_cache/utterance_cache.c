#include "utterance_cache.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "device_pcm_frame.h"
#include "utterance_cache_memory.h"

struct utterance_cache {
    uint8_t *pcm;
    size_t pcm_size;
    size_t source_sample_count;
    char client_input_id[DEVICE_CLIENT_INPUT_ID_MAX_BYTES + 1U];
    device_output_preference_t output_preference;
    uint64_t sealed_at_ms;
    utterance_cache_state_t state;
};

static bool client_input_id_is_valid(const char *client_input_id)
{
    if (client_input_id == NULL) {
        return false;
    }
    const size_t size = strlen(client_input_id);
    if (size == 0U || size > DEVICE_CLIENT_INPUT_ID_MAX_BYTES) {
        return false;
    }
    for (size_t index = 0U; index < size; ++index) {
        if (!isspace((unsigned char)client_input_id[index])) {
            return true;
        }
    }
    return false;
}

static bool output_preference_is_valid(device_output_preference_t output_preference)
{
    return output_preference == DEVICE_OUTPUT_PREFERENCE_TEXT ||
           output_preference == DEVICE_OUTPUT_PREFERENCE_AUDIO ||
           output_preference == DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
}

static void secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size > 0U) {
        *bytes++ = 0U;
        --size;
    }
}

utterance_cache_result_t utterance_cache_create(utterance_cache_t **cache)
{
    if (cache == NULL || *cache != NULL) {
        return UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT;
    }
    utterance_cache_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return UTTERANCE_CACHE_RESULT_NO_MEMORY;
    }
    created->pcm = utterance_cache_memory_allocate(UTTERANCE_CACHE_MAX_PCM_BYTES);
    if (created->pcm == NULL) {
        free(created);
        return UTTERANCE_CACHE_RESULT_NO_MEMORY;
    }
    *cache = created;
    return UTTERANCE_CACHE_RESULT_OK;
}

utterance_cache_result_t utterance_cache_begin(utterance_cache_t *cache,
                                               const char *client_input_id,
                                               device_output_preference_t output_preference)
{
    if (cache == NULL || !client_input_id_is_valid(client_input_id) ||
        !output_preference_is_valid(output_preference)) {
        return UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT;
    }
    if (cache->state != UTTERANCE_CACHE_STATE_EMPTY) {
        return UTTERANCE_CACHE_RESULT_INVALID_STATE;
    }
    memcpy(cache->client_input_id, client_input_id, strlen(client_input_id) + 1U);
    cache->output_preference = output_preference;
    cache->state = UTTERANCE_CACHE_STATE_RECORDING;
    return UTTERANCE_CACHE_RESULT_OK;
}

utterance_cache_result_t utterance_cache_append(utterance_cache_t *cache, const int16_t *samples,
                                                size_t sample_count)
{
    if (cache == NULL || samples == NULL || sample_count == 0U) {
        return UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT;
    }
    if (cache->state != UTTERANCE_CACHE_STATE_RECORDING) {
        return UTTERANCE_CACHE_RESULT_INVALID_STATE;
    }
    if (sample_count > UTTERANCE_CACHE_MAX_SAMPLE_COUNT - cache->source_sample_count) {
        return UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED;
    }
    const size_t byte_count = sample_count * sizeof(*samples);
    memcpy(&cache->pcm[cache->pcm_size], samples, byte_count);
    cache->pcm_size += byte_count;
    cache->source_sample_count += sample_count;
    return UTTERANCE_CACHE_RESULT_OK;
}

utterance_cache_result_t utterance_cache_seal(utterance_cache_t *cache, uint64_t now_ms)
{
    if (cache == NULL) {
        return UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT;
    }
    if (cache->state != UTTERANCE_CACHE_STATE_RECORDING || cache->pcm_size == 0U) {
        return UTTERANCE_CACHE_RESULT_INVALID_STATE;
    }
    const size_t remainder = cache->pcm_size % DEVICE_PCM_PAYLOAD_BYTES;
    if (remainder != 0U) {
        const size_t padding_size = DEVICE_PCM_PAYLOAD_BYTES - remainder;
        if (padding_size > UTTERANCE_CACHE_MAX_PCM_BYTES - cache->pcm_size) {
            return UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED;
        }
        memset(&cache->pcm[cache->pcm_size], 0, padding_size);
        cache->pcm_size += padding_size;
    }
    cache->sealed_at_ms = now_ms;
    cache->state = UTTERANCE_CACHE_STATE_SEALED;
    return UTTERANCE_CACHE_RESULT_OK;
}

utterance_cache_result_t utterance_cache_copy_view(const utterance_cache_t *cache,
                                                   utterance_cache_view_t *view)
{
    if (cache == NULL || view == NULL) {
        return UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT;
    }
    if (cache->state != UTTERANCE_CACHE_STATE_SEALED) {
        return UTTERANCE_CACHE_RESULT_INVALID_STATE;
    }
    *view = (utterance_cache_view_t){
        .pcm = cache->pcm,
        .pcm_size = cache->pcm_size,
        .source_sample_count = cache->source_sample_count,
        .client_input_id = cache->client_input_id,
        .output_preference = cache->output_preference,
        .sealed_at_ms = cache->sealed_at_ms,
    };
    return UTTERANCE_CACHE_RESULT_OK;
}

bool utterance_cache_is_expired(const utterance_cache_t *cache, uint64_t now_ms)
{
    return cache != NULL && cache->state == UTTERANCE_CACHE_STATE_SEALED &&
           now_ms >= cache->sealed_at_ms &&
           now_ms - cache->sealed_at_ms >= UTTERANCE_CACHE_EXPIRY_MS;
}

utterance_cache_state_t utterance_cache_state(const utterance_cache_t *cache)
{
    return cache == NULL ? UTTERANCE_CACHE_STATE_EMPTY : cache->state;
}

void utterance_cache_clear(utterance_cache_t *cache)
{
    if (cache == NULL) {
        return;
    }
    secure_zero(cache->pcm, cache->pcm_size);
    cache->pcm_size = 0U;
    cache->source_sample_count = 0U;
    secure_zero(cache->client_input_id, sizeof(cache->client_input_id));
    cache->output_preference = DEVICE_OUTPUT_PREFERENCE_TEXT;
    cache->sealed_at_ms = 0U;
    cache->state = UTTERANCE_CACHE_STATE_EMPTY;
}

void utterance_cache_destroy(utterance_cache_t *cache)
{
    if (cache == NULL) {
        return;
    }
    utterance_cache_clear(cache);
    utterance_cache_memory_release(cache->pcm);
    cache->pcm = NULL;
    secure_zero(cache, sizeof(*cache));
    free(cache);
}
