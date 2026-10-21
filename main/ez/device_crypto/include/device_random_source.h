#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*device_random_fill_fn)(void *context, uint8_t *output, size_t output_size);

typedef struct {
    device_random_fill_fn fill;
    void *context;
} device_random_source_t;

#ifdef __cplusplus
}
#endif
