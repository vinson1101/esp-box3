#include "utterance_cache_memory.h"

#include "esp_heap_caps.h"

uint8_t *utterance_cache_memory_allocate(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void utterance_cache_memory_release(uint8_t *memory) { heap_caps_free(memory); }
