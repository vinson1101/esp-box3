#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_protocol_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTTERANCE_CACHE_SAMPLE_RATE_HZ 16000U
#define UTTERANCE_CACHE_MAX_DURATION_SECONDS 60U
#define UTTERANCE_CACHE_MAX_SAMPLE_COUNT \
    (UTTERANCE_CACHE_SAMPLE_RATE_HZ * UTTERANCE_CACHE_MAX_DURATION_SECONDS)
#define UTTERANCE_CACHE_MAX_PCM_BYTES (UTTERANCE_CACHE_MAX_SAMPLE_COUNT * 2U)
#define UTTERANCE_CACHE_EXPIRY_MS 120000U

typedef struct utterance_cache utterance_cache_t;

typedef enum {
    UTTERANCE_CACHE_RESULT_OK = 0,
    UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT,
    UTTERANCE_CACHE_RESULT_NO_MEMORY,
    UTTERANCE_CACHE_RESULT_INVALID_STATE,
    UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED,
} utterance_cache_result_t;

typedef enum {
    UTTERANCE_CACHE_STATE_EMPTY = 0,
    UTTERANCE_CACHE_STATE_RECORDING,
    UTTERANCE_CACHE_STATE_SEALED,
} utterance_cache_state_t;

typedef struct {
    const uint8_t *pcm;
    size_t pcm_size;
    size_t source_sample_count;
    const char *client_input_id;
    device_output_preference_t output_preference;
    uint64_t sealed_at_ms;
} utterance_cache_view_t;

utterance_cache_result_t utterance_cache_create(utterance_cache_t **cache);

utterance_cache_result_t utterance_cache_begin(utterance_cache_t *cache,
                                               const char *client_input_id,
                                               device_output_preference_t output_preference);

utterance_cache_result_t utterance_cache_append(utterance_cache_t *cache, const int16_t *samples,
                                                size_t sample_count);

utterance_cache_result_t utterance_cache_seal(utterance_cache_t *cache, uint64_t now_ms);

utterance_cache_result_t utterance_cache_copy_view(const utterance_cache_t *cache,
                                                   utterance_cache_view_t *view);

bool utterance_cache_is_expired(const utterance_cache_t *cache, uint64_t now_ms);

utterance_cache_state_t utterance_cache_state(const utterance_cache_t *cache);

void utterance_cache_clear(utterance_cache_t *cache);

void utterance_cache_destroy(utterance_cache_t *cache);

#ifdef __cplusplus
}
#endif
