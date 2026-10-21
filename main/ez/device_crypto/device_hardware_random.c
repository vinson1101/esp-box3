#include "device_hardware_random.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_random.h"

static bool hardware_random_fill(void *context, uint8_t *output, size_t output_size)
{
    (void)context;
    if (output_size > 0U && output == NULL) {
        return false;
    }
    if (output_size > 0U) {
        esp_fill_random(output, output_size);
    }
    return true;
}

device_random_source_t device_hardware_random_source(void)
{
    return (device_random_source_t) {
        .fill = hardware_random_fill,
    };
}
