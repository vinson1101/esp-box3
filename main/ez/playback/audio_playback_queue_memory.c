#include "audio_playback_queue_memory.h"

#include "esp_heap_caps.h"

uint8_t *audio_playback_queue_memory_allocate(size_t size)
{
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void audio_playback_queue_memory_release(uint8_t *memory)
{
    heap_caps_free(memory);
}
