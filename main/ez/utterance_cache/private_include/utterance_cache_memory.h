#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t *utterance_cache_memory_allocate(size_t size);

void utterance_cache_memory_release(uint8_t *memory);
