#pragma once

#include <cstdint>

struct Box3SensorState {
    bool initialization_error = false;
    bool radar_initializing = false;
    bool temperature_connected = false;
    bool temperature_valid = false;
    bool radar_connected = false;
    bool radar_valid = false;
    bool motion = false;
    float temperature = 0;
    float humidity = 0;
    uint64_t updated_ms = 0;
    uint64_t now_ms = 0;
};
