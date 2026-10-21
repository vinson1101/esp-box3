#include "audio_playback_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback_queue_memory.h"

#define TEST_CONNECTION_EPOCH 7U

static uint8_t *allocated_memory;
static size_t allocated_size;

uint8_t *audio_playback_queue_memory_allocate(size_t size)
{
    assert(allocated_memory == NULL);
    allocated_memory = calloc(1U, size);
    allocated_size = size;
    return allocated_memory;
}

void audio_playback_queue_memory_release(uint8_t *memory)
{
    assert(memory == allocated_memory);
    for (size_t index = 0U; index < allocated_size; ++index) {
        assert(memory[index] == 0U);
    }
    free(memory);
    allocated_memory = NULL;
    allocated_size = 0U;
}

static void encode_sample(uint8_t *destination, int16_t sample)
{
    const uint16_t encoded = (uint16_t)sample;
    destination[0] = (uint8_t)encoded;
    destination[1] = (uint8_t)(encoded >> 8U);
}

static device_media_playback_start_t playback_start(
    uint32_t stream_id,
    const char *output_id,
    uint64_t sample_count)
{
    device_media_playback_start_t start = {
        .stream_id = stream_id,
        .sample_count = sample_count,
    };
    snprintf(start.output_id, sizeof(start.output_id), "%s", output_id);
    return start;
}

static device_media_playback_end_t playback_end(
    uint32_t stream_id,
    const char *output_id)
{
    device_media_playback_end_t end = {.stream_id = stream_id};
    snprintf(end.output_id, sizeof(end.output_id), "%s", output_id);
    snprintf(end.reason, sizeof(end.reason), "completed");
    return end;
}

static device_pcm_frame_view_t frame(
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES],
    uint32_t stream_id,
    uint32_t sequence,
    int16_t first_sample)
{
    memset(payload, 0, DEVICE_PCM_PAYLOAD_BYTES);
    encode_sample(payload, first_sample);
    return (device_pcm_frame_view_t){
        .direction = DEVICE_PCM_DIRECTION_DOWNLINK,
        .stream_id = stream_id,
        .sequence = sequence,
        .payload = payload,
        .payload_size = DEVICE_PCM_PAYLOAD_BYTES,
    };
}

static void push_frame(
    audio_playback_queue_t *queue,
    uint32_t stream_id,
    uint32_t sequence,
    int16_t first_sample)
{
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    const device_pcm_frame_view_t current =
        frame(payload, stream_id, sequence, first_sample);
    assert(audio_playback_queue_push(queue, TEST_CONNECTION_EPOCH, &current) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
}

static void verify_sequence_and_partial_tail(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    const device_media_playback_start_t start = playback_start(9U, "output-a", 321U);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 9U, 0U, INT16_MIN);
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    const device_pcm_frame_view_t wrong_sequence = frame(payload, 9U, 2U, 5);
    assert(audio_playback_queue_push(queue, TEST_CONNECTION_EPOCH, &wrong_sequence) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_SEQUENCE_MISMATCH);
    push_frame(queue, 9U, 1U, INT16_MAX);
    const device_media_playback_end_t end = playback_end(9U, "output-a");
    assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH, &end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(audio_playback_queue_head_ready(queue, 4U));

    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME];
    size_t sample_count = 0U;
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(sample_count == AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(samples[0] == INT16_MIN);
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(sample_count == 1U);
    assert(samples[0] == INT16_MAX);
    assert(samples[1] == 0);
    assert(audio_playback_queue_head_drained(queue));
    assert(audio_playback_queue_complete_head(queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    audio_playback_queue_status_t status = {0};
    audio_playback_queue_get_status(queue, &status);
    assert(status.stream_count == 0U);
    audio_playback_queue_destroy(queue);
}

static void verify_multiple_streams_share_one_fifo(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    device_media_playback_start_t first_start = playback_start(
        10U, "output-first", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &first_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 10U, 0U, 101);
    device_media_playback_end_t first_end = playback_end(10U, "output-first");
    assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH, &first_end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);

    device_media_playback_start_t second_start = playback_start(
        11U, "output-second", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &second_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 11U, 0U, 202);
    device_media_playback_end_t second_end = playback_end(11U, "output-second");
    assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH, &second_end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);

    audio_playback_queue_status_t status = {0};
    audio_playback_queue_get_status(queue, &status);
    assert(status.stream_id == 10U);
    assert(status.stream_count == 2U);
    assert(status.queued_frame_count == 1U);

    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME];
    size_t sample_count = 0U;
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(samples[0] == 101);
    assert(audio_playback_queue_head_drained(queue));
    assert(audio_playback_queue_complete_head(queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    audio_playback_queue_get_status(queue, &status);
    assert(status.stream_id == 11U);
    assert(status.stream_count == 1U);
    assert(status.queued_frame_count == 1U);
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(samples[0] == 202);
    assert(audio_playback_queue_head_drained(queue));
    assert(audio_playback_queue_complete_head(queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    audio_playback_queue_destroy(queue);
}

static void verify_global_frame_capacity(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    const uint64_t sample_count =
        (uint64_t)(AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY + 1U) *
        AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME;
    device_media_playback_start_t start = playback_start(12U, "output-capacity", sample_count);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    for (uint32_t sequence = 0U; sequence < AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY; ++sequence) {
        push_frame(queue, 12U, sequence, (int16_t)sequence);
    }
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    const device_pcm_frame_view_t overflow =
        frame(payload, 12U, AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY, 0);
    assert(audio_playback_queue_push(queue, TEST_CONNECTION_EPOCH, &overflow) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_FRAME_CAPACITY_EXCEEDED);
    audio_playback_queue_status_t status = {0};
    audio_playback_queue_get_status(queue, &status);
    assert(status.total_queued_frame_count == AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY);
    assert(status.frame_high_watermark == AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY);
    audio_playback_queue_clear(queue);
    audio_playback_queue_get_status(queue, &status);
    assert(status.total_queued_frame_count == 0U);
    assert(status.frame_high_watermark == AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY);
    assert(status.stream_high_watermark == 1U);
    audio_playback_queue_destroy(queue);
}

static void verify_stream_descriptor_capacity(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    for (uint32_t index = 0U; index < AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY; ++index) {
        const uint32_t stream_id = 100U + index;
        char output_id[32];
        snprintf(output_id, sizeof(output_id), "output-%lu", (unsigned long)index);
        device_media_playback_start_t start = playback_start(
            stream_id, output_id, AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
        assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &start) ==
               AUDIO_PLAYBACK_QUEUE_RESULT_OK);
        push_frame(queue, stream_id, 0U, (int16_t)index);
        device_media_playback_end_t end = playback_end(stream_id, output_id);
        assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH, &end) ==
               AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    }
    device_media_playback_start_t overflow = playback_start(
        200U, "output-overflow", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &overflow) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_CAPACITY_EXCEEDED);
    audio_playback_queue_status_t status = {0};
    audio_playback_queue_get_status(queue, &status);
    assert(status.stream_high_watermark == AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY);
    assert(status.total_queued_frame_count == AUDIO_PLAYBACK_QUEUE_STREAM_CAPACITY);
    assert(status.queued_frame_count == 1U);
    audio_playback_queue_destroy(queue);
}

static void verify_tail_discard_preserves_head(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    device_media_playback_start_t first_start = playback_start(
        13U, "output-head", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &first_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 13U, 0U, 303);
    device_media_playback_end_t first_end = playback_end(13U, "output-head");
    assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH, &first_end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);

    device_media_playback_start_t tail_start = playback_start(
        14U, "output-tail", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &tail_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 14U, 0U, 404);
    device_media_playback_end_t tail_end = playback_end(14U, "output-tail");
    assert(audio_playback_queue_discard_tail(queue, TEST_CONNECTION_EPOCH, &tail_end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);

    audio_playback_queue_status_t status = {0};
    audio_playback_queue_get_status(queue, &status);
    assert(status.stream_id == 13U);
    assert(status.stream_count == 1U);
    assert(status.queued_frame_count == 1U);
    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME];
    size_t sample_count = 0U;
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(samples[0] == 303);
    audio_playback_queue_destroy(queue);
}

static void verify_clear_reuses_queue_without_exposing_stale_frames(void)
{
    audio_playback_queue_t *queue = NULL;
    assert(audio_playback_queue_create(&queue) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    device_media_playback_start_t old_start = playback_start(
        15U, "output-old", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH, &old_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    push_frame(queue, 15U, 0U, 505);
    audio_playback_queue_clear(queue);

    device_media_playback_start_t replacement_start = playback_start(
        16U, "output-new", AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME);
    assert(audio_playback_queue_begin(queue, TEST_CONNECTION_EPOCH + 1U, &replacement_start) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    device_pcm_frame_view_t replacement = frame(payload, 16U, 0U, 606);
    assert(audio_playback_queue_push(queue, TEST_CONNECTION_EPOCH + 1U, &replacement) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    device_media_playback_end_t replacement_end = playback_end(16U, "output-new");
    assert(audio_playback_queue_end(queue, TEST_CONNECTION_EPOCH + 1U, &replacement_end) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    int16_t samples[AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME];
    size_t sample_count = 0U;
    assert(audio_playback_queue_pop(queue, samples, &sample_count) ==
           AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(samples[0] == 606);
    audio_playback_queue_destroy(queue);
}

int main(void)
{
    verify_sequence_and_partial_tail();
    verify_multiple_streams_share_one_fifo();
    verify_global_frame_capacity();
    verify_stream_descriptor_capacity();
    verify_tail_discard_preserves_head();
    verify_clear_reuses_queue_without_exposing_stale_frames();
    puts("audio playback queue ordering, capacity, and isolation checks passed");
    return 0;
}
