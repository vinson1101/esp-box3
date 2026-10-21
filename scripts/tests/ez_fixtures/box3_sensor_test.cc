#include "box3_sensor.h"
#include <cmath>

static void Checksum() {
    uint8_t crc = 0xff;
    for (int i = 0; i < 6; ++i) {
        crc ^= packet[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
    packet[6] = crc;
}

int main() {
    Box3Sensor sensor;
    temperature_present = radar_present = false;
    auto state = sensor.Read();
    assert(!state.temperature_connected && !state.radar_connected && state.updated_ms == 0);
    temperature_present = radar_present = true;
    Checksum();
    state = sensor.Read();
    assert(state.temperature_valid && std::fabs(state.temperature - 25) < 0.001);
    assert(std::fabs(state.humidity - 50) < 0.001 && state.updated_ms > 0);
    assert(state.radar_connected && !state.radar_valid &&
           state.radar_initializing);  // Ignore power-on self-check output.
    const std::vector<std::array<uint8_t, 2>> expected = {
        {0x68, 0x68}, {0x67, 0x8b}, {0x10, 200},  {0x11, 0}, {0x5c, 0x1b}, {0x3d, 0xf4},
        {0x3e, 1},    {0x3f, 0},    {0x40, 0},    {0x41, 1}, {0x42, 0xdc}, {0x43, 5},
        {0x44, 0},    {0x45, 0},    {0x38, 0xd0}, {0x39, 7}, {0x4e, 0xe8}, {0x4f, 3},
        {0x55, 4},    {0, 0},       {0, 1}};
    assert(radar_writes == expected);
    time_us += 2000000;
    state = sensor.Read();
    assert(state.radar_valid && state.motion);
    assert(radar_writes == expected);  // No reconfiguration each sample.
    radar_level = 0;
    state = sensor.Read();
    assert(state.radar_valid && !state.motion);
    const auto updated = state.updated_ms;
    packet[6] ^= 1;
    state = sensor.Read();
    assert(!state.temperature_valid && state.updated_ms == updated && state.radar_valid);
    packet[0] = 0x98;
    Checksum();
    assert(!sensor.Read().temperature_valid);  // Busy is never a valid sample.
    packet[0] = 0x10;
    Checksum();
    assert(!sensor.Read().temperature_valid);  // Uncalibrated is not a measurement.
    packet[0] = 0x18;
    Checksum();
    write_fail = true;
    assert(!sensor.Read().temperature_valid);
    write_fail = false;
    receive_fail = true;
    assert(!sensor.Read().temperature_valid);
    receive_fail = false;
    radar_present = false;
    state = sensor.Read();
    assert(!state.radar_valid && state.temperature_valid);
    radar_present = true;
    radar_fail = true;
    assert(!sensor.Read().radar_valid);
    radar_fail = false;
    state = sensor.Read();
    assert(!state.radar_valid && radar_writes.size() == 2 * expected.size());
    time_us += 2000000;
    assert(sensor.Read().radar_valid);
    temperature_present = radar_present = false;
    state = sensor.Read();
    assert(!state.temperature_valid &&
           !state.radar_valid);  // Never reuse stale motion/temperature.

    Box3Sensor failure;
    bus_fail = true;
    assert(failure.Read().initialization_error);
    bus_fail = false;
    add_fail = true;
    failure.Read();
    assert(removed == 1 && bus_deleted == 1);
    add_fail = false;
    gpio_fail = true;
    failure.Read();
    assert(removed == 3 && bus_deleted == 2);
    gpio_fail = false;
    temperature_present = radar_present = true;
    assert(failure.Read().temperature_valid);  // Retry after resource or GPIO failure.
}
