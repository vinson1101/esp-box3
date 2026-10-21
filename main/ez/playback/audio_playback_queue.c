#include "audio_playback_queue.h"

#include <stdlib.h>
#include <string.h>

#include "audio_playback_queue_memory.h"

#define AUDIO_PLAYBACK_QUEUE_STORAGE_BYTES \
    (AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY * DEVICE_PCM_PAYLOAD_BYTES)

typedef struct {
    uint64_t connection_epoch;
    uint32_t stream_id;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    uint64_t sample_count;
    uint64_t required_frame_count;
    uint64_t received_frame_count;
    uint64_t consumed_frame_count;
    uint64_t consumed_sample_count;
    bool ended;
} audio_playback_stream_t;

struct audio_playback_queue {
    uint8_t *storage;
    audio_playback_stream_t streams[AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY];
    size_t frame_head;
    size_t queued_frame_count;
    size_t stream_head;
    size_t stream_count;
    size_t frame_high_watermark;
    size_t stream_high_watermark;
};

static void secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size > 0U) {
        *bytes++ = 0U;
        --size;
    }
}

static bool identifier_is_terminated(const char *identifier, size_t capacity)
{
    return identifier != NULL && identifier[0] != '\0' &&
           memchr(identifier, '\0', capacity) != NULL;
}

static audio_playback_stream_t *head_stream(audio_playback_queue_t *queue)
{
    return queue->stream_count == 0U ? NULL : &queue->streams[queue->stream_head];
}

static const audio_playback_stream_t *const_head_stream(const audio_playback_queue_t *queue)
{
    return queue->stream_count == 0U ? NULL : &queue->streams[queue->stream_head];
}

static size_t tail_stream_index(const audio_playback_queue_t *queue)
{
    return (queue->stream_head + queue->stream_count - 1U) %
           AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY;
}

static audio_playback_stream_t *tail_stream(audio_playback_queue_t *queue)
{
    return queue->stream_count == 0U ? NULL : &queue->streams[tail_stream_index(queue)];
}

static const audio_playback_stream_t *const_tail_stream(const audio_playback_queue_t *queue)
{
    return queue->stream_count == 0U ? NULL : &queue->streams[tail_stream_index(queue)];
}

static bool stream_matches(
    const audio_playback_stream_t *stream,
    uint64_t connection_epoch,
    uint32_t stream_id)
{
    return stream != NULL && stream->connection_epoch == connection_epoch &&
           stream->stream_id == stream_id;
}

static size_t stream_queued_frame_count(const audio_playback_stream_t *stream)
{
    return (size_t)(stream->received_frame_count - stream->consumed_frame_count);
}

static int16_t decode_pcm16_little_endian(const uint8_t *encoded)
{
    const uint16_t raw = (uint16_t)encoded[0] | ((uint16_t)encoded[1] << 8U);
    const int32_t signed_sample =
        raw >= UINT16_C(0x8000) ? (int32_t)raw - INT32_C(0x10000) : (int32_t)raw;
    return (int16_t)signed_sample;
}

audio_playback_queue_result_t audio_playback_queue_create(audio_playback_queue_t **queue)
{
    if (queue == NULL || *queue != NULL) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_queue_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_NO_MEMORY;
    }
    created->storage = audio_playback_queue_memory_allocate(AUDIO_PLAYBACK_QUEUE_STORAGE_BYTES);
    if (created->storage == NULL) {
        free(created);
        return AUDIO_PLAYBACK_QUEUE_RESULT_NO_MEMORY;
    }
    *queue = created;
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

audio_playback_queue_result_t audio_playback_queue_begin(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_start_t *playback_start)
{
    if (queue == NULL || connection_epoch == 0U || playback_start == NULL ||
        playback_start->stream_id == 0U || playback_start->sample_count == 0U ||
        !identifier_is_terminated(playback_start->output_id,
                                  sizeof(playback_start->output_id))) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    const audio_playback_stream_t *current_tail = const_tail_stream(queue);
    if (current_tail != NULL && !current_tail->ended) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    if (queue->stream_count == AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_CAPACITY_EXCEEDED;
    }
    const uint64_t required_frame_count =
        playback_start->sample_count / AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME +
        (playback_start->sample_count % AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME == 0U ? 0U : 1U);
    if (required_frame_count > UINT32_MAX) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    const size_t index = (queue->stream_head + queue->stream_count) %
                         AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY;
    audio_playback_stream_t *stream = &queue->streams[index];
    *stream = (audio_playback_stream_t){
        .connection_epoch = connection_epoch,
        .stream_id = playback_start->stream_id,
        .sample_count = playback_start->sample_count,
        .required_frame_count = required_frame_count,
    };
    memcpy(stream->output_id, playback_start->output_id, sizeof(stream->output_id));
    ++queue->stream_count;
    if (queue->stream_count > queue->stream_high_watermark) {
        queue->stream_high_watermark = queue->stream_count;
    }
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

audio_playback_queue_result_t audio_playback_queue_push(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_pcm_frame_view_t *frame)
{
    if (queue == NULL || connection_epoch == 0U || frame == NULL ||
        frame->direction != DEVICE_PCM_DIRECTION_DOWNLINK || frame->stream_id == 0U ||
        frame->payload == NULL || frame->payload_size != DEVICE_PCM_PAYLOAD_BYTES) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_stream_t *stream = tail_stream(queue);
    if (stream == NULL || stream->ended) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    if (!stream_matches(stream, connection_epoch, frame->stream_id)) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_MISMATCH;
    }
    if ((uint64_t)frame->sequence != stream->received_frame_count) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_SEQUENCE_MISMATCH;
    }
    if (stream->received_frame_count >= stream->required_frame_count) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_SAMPLE_COUNT_EXCEEDED;
    }
    if (queue->queued_frame_count == AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_FRAME_CAPACITY_EXCEEDED;
    }
    const size_t frame_tail =
        (queue->frame_head + queue->queued_frame_count) % AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY;
    memcpy(&queue->storage[frame_tail * DEVICE_PCM_PAYLOAD_BYTES], frame->payload,
           DEVICE_PCM_PAYLOAD_BYTES);
    ++queue->queued_frame_count;
    if (queue->queued_frame_count > queue->frame_high_watermark) {
        queue->frame_high_watermark = queue->queued_frame_count;
    }
    ++stream->received_frame_count;
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

audio_playback_queue_result_t audio_playback_queue_end(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end)
{
    if (queue == NULL || connection_epoch == 0U || playback_end == NULL ||
        playback_end->stream_id == 0U ||
        !identifier_is_terminated(playback_end->output_id, sizeof(playback_end->output_id))) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_stream_t *stream = tail_stream(queue);
    if (stream == NULL || stream->ended) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    if (!stream_matches(stream, connection_epoch, playback_end->stream_id) ||
        strcmp(stream->output_id, playback_end->output_id) != 0) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_MISMATCH;
    }
    if (stream->received_frame_count != stream->required_frame_count) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INCOMPLETE;
    }
    stream->ended = true;
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

audio_playback_queue_result_t audio_playback_queue_pop(
    audio_playback_queue_t *queue,
    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME],
    size_t *sample_count)
{
    if (queue == NULL || samples == NULL || sample_count == NULL) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_stream_t *stream = head_stream(queue);
    if (stream == NULL) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    if (stream_queued_frame_count(stream) == 0U) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_EMPTY;
    }
    const uint8_t *encoded = &queue->storage[queue->frame_head * DEVICE_PCM_PAYLOAD_BYTES];
    const uint64_t remaining_samples = stream->sample_count - stream->consumed_sample_count;
    const size_t valid_samples = remaining_samples < AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME
                                     ? (size_t)remaining_samples
                                     : AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME;
    for (size_t index = 0U; index < valid_samples; ++index) {
        samples[index] = decode_pcm16_little_endian(&encoded[index * 2U]);
    }
    if (valid_samples < AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME) {
        memset(&samples[valid_samples], 0,
               (AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME - valid_samples) * sizeof(samples[0]));
    }
    secure_zero(&queue->storage[queue->frame_head * DEVICE_PCM_PAYLOAD_BYTES],
                DEVICE_PCM_PAYLOAD_BYTES);
    queue->frame_head = (queue->frame_head + 1U) % AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY;
    --queue->queued_frame_count;
    ++stream->consumed_frame_count;
    stream->consumed_sample_count += valid_samples;
    *sample_count = valid_samples;
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

bool audio_playback_queue_head_ready(
    const audio_playback_queue_t *queue,
    size_t prebuffer_frame_count)
{
    const audio_playback_stream_t *stream =
        queue == NULL ? NULL : const_head_stream(queue);
    if (stream == NULL || prebuffer_frame_count == 0U ||
        prebuffer_frame_count > AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY) {
        return false;
    }
    const size_t queued_frame_count = stream_queued_frame_count(stream);
    return queued_frame_count >= prebuffer_frame_count ||
           (stream->ended && queued_frame_count > 0U);
}

bool audio_playback_queue_head_drained(const audio_playback_queue_t *queue)
{
    const audio_playback_stream_t *stream =
        queue == NULL ? NULL : const_head_stream(queue);
    return stream != NULL && stream->ended &&
           stream->consumed_sample_count == stream->sample_count;
}

audio_playback_queue_result_t audio_playback_queue_complete_head(
    audio_playback_queue_t *queue)
{
    if (queue == NULL) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_stream_t *stream = head_stream(queue);
    if (stream == NULL || !stream->ended ||
        stream->consumed_sample_count != stream->sample_count) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    secure_zero(stream, sizeof(*stream));
    queue->stream_head = (queue->stream_head + 1U) % AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY;
    --queue->stream_count;
    if (queue->stream_count == 0U) {
        queue->stream_head = 0U;
        queue->frame_head = 0U;
    }
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

audio_playback_queue_result_t audio_playback_queue_discard_tail(
    audio_playback_queue_t *queue,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end)
{
    if (queue == NULL || connection_epoch == 0U || playback_end == NULL ||
        playback_end->stream_id == 0U ||
        !identifier_is_terminated(playback_end->output_id, sizeof(playback_end->output_id))) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT;
    }
    if (queue->stream_count < 2U) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    audio_playback_stream_t *stream = tail_stream(queue);
    if (!stream_matches(stream, connection_epoch, playback_end->stream_id) ||
        strcmp(stream->output_id, playback_end->output_id) != 0) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_MISMATCH;
    }
    const size_t discarded_frame_count = stream_queued_frame_count(stream);
    if (discarded_frame_count > queue->queued_frame_count) {
        return AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE;
    }
    queue->queued_frame_count -= discarded_frame_count;
    secure_zero(stream, sizeof(*stream));
    --queue->stream_count;
    return AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

void audio_playback_queue_get_status(
    const audio_playback_queue_t *queue,
    audio_playback_queue_status_t *status)
{
    if (queue == NULL || status == NULL) {
        return;
    }
    /* Lifetime peaks survive queue drains and cancellation, including idle snapshots. */
    *status = (audio_playback_queue_status_t){
        .total_queued_frame_count = queue->queued_frame_count,
        .frame_high_watermark = queue->frame_high_watermark,
        .stream_high_watermark = queue->stream_high_watermark,
    };
    const audio_playback_stream_t *stream = const_head_stream(queue);
    if (stream == NULL) {
        return;
    }
    *status = (audio_playback_queue_status_t){
        .connection_epoch = stream->connection_epoch,
        .stream_id = stream->stream_id,
        .sample_count = stream->sample_count,
        .received_frame_count = stream->received_frame_count,
        .consumed_sample_count = stream->consumed_sample_count,
        .queued_frame_count = stream_queued_frame_count(stream),
        .stream_count = queue->stream_count,
        .ended = stream->ended,
        .total_queued_frame_count = queue->queued_frame_count,
        .frame_high_watermark = queue->frame_high_watermark,
        .stream_high_watermark = queue->stream_high_watermark,
    };
    memcpy(status->output_id, stream->output_id, sizeof(status->output_id));
}

void audio_playback_queue_clear(audio_playback_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    secure_zero(queue->streams, sizeof(queue->streams));
    queue->frame_head = 0U;
    queue->queued_frame_count = 0U;
    queue->stream_head = 0U;
    queue->stream_count = 0U;
}

void audio_playback_queue_destroy(audio_playback_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    audio_playback_queue_clear(queue);
    secure_zero(queue->storage, AUDIO_PLAYBACK_QUEUE_STORAGE_BYTES);
    audio_playback_queue_memory_release(queue->storage);
    queue->storage = NULL;
    secure_zero(queue, sizeof(*queue));
    free(queue);
}
