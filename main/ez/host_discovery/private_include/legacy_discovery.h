#ifndef LEGACY_DISCOVERY_H
#define LEGACY_DISCOVERY_H

#include "host_discovery.h"

host_discovery_result_t legacy_discovery_parse(const uint8_t* packet, size_t length,
                                               uint32_t source_address,
                                               host_discovery_record_t* records, size_t capacity,
                                               size_t* count);

host_discovery_result_t legacy_discovery_query(uint32_t timeout_ms,
                                               host_discovery_record_t* records, size_t capacity,
                                               size_t* count);

#endif
