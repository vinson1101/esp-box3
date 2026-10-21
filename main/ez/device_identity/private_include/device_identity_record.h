#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_identity.h"

#define DEVICE_IDENTITY_RECORD_SCHEMA_VERSION 1U
#define DEVICE_IDENTITY_RECORD_ENCODED_BYTES 435U

device_identity_result_t device_identity_record_encode(
    const device_identity_record_t *record,
    uint8_t *encoded,
    size_t encoded_capacity,
    size_t *encoded_size);

device_identity_result_t device_identity_record_decode(
    const uint8_t *encoded,
    size_t encoded_size,
    device_identity_record_t *record);
