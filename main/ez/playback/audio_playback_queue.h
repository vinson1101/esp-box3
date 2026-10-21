#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_media_control.h"
#include "device_pcm_frame.h"

#define AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY 256U
#define AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY 8U
#define AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME (DEVICE_PCM_PAYLOAD_BYTES / 2U)

typedef struct audio_playback_queue audio_playback_queue_t;

typedef enum {
    AUDIO_PLAYBACK_QUEUE_RESULT_OK = 0,
    AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT,
    AUDIO_PLAYBACK_QUEUE_RESULT_NO_MEMORY,
    AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE,
    AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_MISMATCH,
    AUDIO_PLAYBACK_QUEUE_RESULT_SEQUENCE_MISMATCH,
    AUDIO_PLAYBACK_QUEUE_RESULT_FRAME_CAPACITY_EXCEEDED,
    AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_CAPACITY_EXCEEDED,
    AUDIO_PLAYBACK_QUEUE_RESULT_SAMPLE_COUNT_EXCEEDED,
    AUDIO_PLAYBACK_QUEUE_RESULT_EMPTY,
    AUDIO_PLAYBACK_QUEUE_RESULT_INCOMPLETE,
} audio_playback_queue_result_t;

typedef struct {
    uint64_t connection_epoch;
    uint32_t stream_id;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    uint64_t sample_count;
    uint64_t received_frame_count;
    uint64_t consumed_sample_count;
    size_t queued_frame_count;
    size_t stream_count;
    bool ended;
    size_t total_queued_frame_count;
    size_t frame_high_watermark;
    size_t stream_high_watermark;
} audio_playback_queue_status_t;

audio_playback_queue_result_t audio_playback_queue_create(audio_playback_queue_t **queue);

audio_playback_queue_result_t audio_playback_queue_begin(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_start_t *playback_start);

audio_playback_queue_result_t audio_playback_queue_push(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_pcm_frame_view_t *frame);

audio_playback_queue_result_t audio_playback_queue_end(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end);

audio_playback_queue_result_t audio_playback_queue_pop(
    audio_playback_queue_t *queue,
    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME],
    size_t *sample_count);

bool audio_playback_queue_head_ready(
    const audio_playback_queue_t *queue,
    size_t prebuffer_frame_count);

bool audio_playback_queue_head_drained(const audio_playback_queue_t *queue);

audio_playback_queue_result_t audio_playback_queue_complete_head(
    audio_playback_queue_t *queue);

audio_playback_queue_result_t audio_playback_queue_discard_tail(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end);

void audio_playback_queue_get_status(
    const audio_playback_queue_t *queue,
    audio_playback_queue_status_t *status);

void audio_playback_queue_clear(audio_playback_queue_t *queue);

void audio_playback_queue_destroy(audio_playback_queue_t *queue);
