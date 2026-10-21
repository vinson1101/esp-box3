#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, GPIO_NUM_41 = 41, GPIO_NUM_40 = 40, GPIO_NUM_21 = 21;
constexpr int I2C_NUM_0 = 0, I2C_CLK_SRC_DEFAULT = 0, I2C_ADDR_BIT_LEN_7 = 0;
constexpr int GPIO_MODE_INPUT = 0, GPIO_PULLUP_ENABLE = 1;
struct Device {
    uint8_t address;
};
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = Device*;
struct i2c_master_bus_config_t {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct {
        bool enable_internal_pullup;
    } flags;
};
struct i2c_device_config_t {
    int dev_addr_length, scl_speed_hz, device_address;
};
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en;
};
static int64_t time_us = 1000000;
static bool temperature_present = true, radar_present = true;
static bool bus_fail = false, add_fail = false, gpio_fail = false;
static bool write_fail = false, receive_fail = false, radar_fail = false;
static int radar_level = 1, removed = 0, bus_deleted = 0;
static Device temperature_device{0x38}, radar_device{0x28};
static std::vector<std::array<uint8_t, 2>> radar_writes;
static uint8_t packet[] = {0x18, 0x80, 0x00, 0x06, 0x00, 0x00, 0x00};
inline int i2c_new_master_bus(const i2c_master_bus_config_t* c, i2c_master_bus_handle_t* out) {
    assert(c->i2c_port == 0 && c->sda_io_num == 41 && c->scl_io_num == 40);
    if (bus_fail)
        return -1;
    *out = &temperature_device;
    return ESP_OK;
}
inline int i2c_master_bus_add_device(void*, const i2c_device_config_t* c, Device** out) {
    assert(c->scl_speed_hz == 100000);
    if (add_fail && c->device_address == 0x28)
        return -1;
    *out = c->device_address == 0x38 ? &temperature_device : &radar_device;
    return ESP_OK;
}
inline int i2c_master_bus_rm_device(Device*) {
    ++removed;
    return 0;
}
inline int i2c_del_master_bus(void*) {
    ++bus_deleted;
    return 0;
}
inline int gpio_config(const gpio_config_t* c) {
    assert(c->pin_bit_mask == (1ULL << 21) && c->pull_up_en == 1);
    return gpio_fail ? -1 : 0;
}
inline int gpio_get_level(int pin) {
    assert(pin == 21);
    return radar_level;
}
inline int i2c_master_probe(void*, uint8_t address, int timeout) {
    assert(timeout == 100);
    return (address == 0x38 ? temperature_present : radar_present) ? ESP_OK : -1;
}
inline int i2c_master_transmit(Device* d, const uint8_t* p, size_t n, int timeout) {
    assert(timeout == 100);
    if (d->address == 0x28) {
        assert(n == 2);
        if (radar_fail)
            return -1;
        radar_writes.push_back({p[0], p[1]});
    } else {
        assert(n == 3 && p[0] == 0xac && p[1] == 0x33 && p[2] == 0);
        if (write_fail)
            return -1;
    }
    return ESP_OK;
}
inline int i2c_master_receive(Device*, uint8_t* out, size_t n, int timeout) {
    assert(n == 7 && timeout == 100);
    if (receive_fail)
        return -1;
    std::memcpy(out, packet, n);
    return 0;
}
inline int64_t esp_timer_get_time() { return time_us; }
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(int ms) { time_us += int64_t(ms) * 1000; }
inline const char* esp_err_to_name(int) { return "fake error"; }
#define ESP_LOGW(...) ((void)0)
