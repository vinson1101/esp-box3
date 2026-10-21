/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * AHT20 decoding/CRC and AT581X default initialization adapted to IDF 6.1 from
 * espressif/esp-iot-solution @ 4962410c80a51cd5822da1d8d2041eb4285265f1.
 * BOX-3 pins and polling follow espressif/esp-box @ aae1b7a6d193d968cf53e42f291a60c4ca1c622c.
 */
#pragma once

#include "box3_sensor_state.h"
#include "config.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Box3Sensor {
public:
    Box3SensorState Read() {
        Box3SensorState state;
        state.updated_ms = updated_ms_;
        state.now_ms = esp_timer_get_time() / 1000;
        if (!Initialize()) {
            state.initialization_error = true;
            return state;
        }

        state.radar_connected = i2c_master_probe(bus_, 0x28, kTimeoutMs) == ESP_OK;
        if (!state.radar_connected) {
            radar_ready_ = false;
        } else if (!radar_ready_) {
            radar_ready_ = InitializeRadar();
            radar_ready_ms_ = esp_timer_get_time() / 1000 + 2000;
        } else if (state.now_ms >= radar_ready_ms_) {
            state.radar_valid = true;
            state.motion = gpio_get_level(SENSOR_RADAR_OUT_PIN) != 0;
        }

        state.radar_initializing = state.radar_connected && radar_ready_ && !state.radar_valid;
        state.temperature_connected = i2c_master_probe(bus_, 0x38, kTimeoutMs) == ESP_OK;
        if (state.temperature_connected) {
            state.temperature_valid = ReadTemperature(&state.temperature, &state.humidity);
            if (state.temperature_valid)
                updated_ms_ = esp_timer_get_time() / 1000;
        }
        state.updated_ms = updated_ms_;
        state.now_ms = esp_timer_get_time() / 1000;
        return state;
    }

private:
    static constexpr int kTimeoutMs = 100;
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t temperature_ = nullptr;
    i2c_master_dev_handle_t radar_ = nullptr;
    bool radar_ready_ = false;
    uint64_t radar_ready_ms_ = 0;
    uint64_t updated_ms_ = 0;

    bool Initialize() {
        if (bus_)
            return true;
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_0;
        config.sda_io_num = SENSOR_I2C_SDA_PIN;
        config.scl_io_num = SENSOR_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = true;
        esp_err_t result = i2c_new_master_bus(&config, &bus_);
        if (result != ESP_OK) {
            bus_ = nullptr;
            ESP_LOGW("Box3Sensor", "Expansion bus unavailable: %s", esp_err_to_name(result));
            return false;
        }
        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.scl_speed_hz = 100000;
        device.device_address = 0x38;
        result = i2c_master_bus_add_device(bus_, &device, &temperature_);
        if (result == ESP_OK) {
            device.device_address = 0x28;
            result = i2c_master_bus_add_device(bus_, &device, &radar_);
        }
        gpio_config_t gpio = {};
        gpio.pin_bit_mask = 1ULL << SENSOR_RADAR_OUT_PIN;
        gpio.mode = GPIO_MODE_INPUT;
        gpio.pull_up_en = GPIO_PULLUP_ENABLE;
        if (result == ESP_OK)
            result = gpio_config(&gpio);
        if (result == ESP_OK)
            return true;
        ESP_LOGW("Box3Sensor", "Expansion initialization failed: %s", esp_err_to_name(result));
        if (radar_)
            i2c_master_bus_rm_device(radar_);
        if (temperature_)
            i2c_master_bus_rm_device(temperature_);
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
        temperature_ = nullptr;
        radar_ = nullptr;
        return false;
    }

    bool InitializeRadar() {
        // Official defaults: 70 uA, delta 200, gain 3, base/hold 500/1500 ms,
        // self-check 2000 ms, protection 1000 ms. No system sleep ownership.
        static const uint8_t registers[][2] = {
            {0x68, 0x68}, {0x67, 0x8b}, {0x10, 0xc8}, {0x11, 0x00}, {0x5c, 0x1b}, {0x3d, 0xf4},
            {0x3e, 0x01}, {0x3f, 0x00}, {0x40, 0x00}, {0x41, 0x01}, {0x42, 0xdc}, {0x43, 0x05},
            {0x44, 0x00}, {0x45, 0x00}, {0x38, 0xd0}, {0x39, 0x07}, {0x4e, 0xe8}, {0x4f, 0x03},
            {0x55, 0x04}, {0x00, 0x00}, {0x00, 0x01},
        };
        for (const auto& value : registers) {
            if (i2c_master_transmit(radar_, value, sizeof(value), kTimeoutMs) != ESP_OK)
                return false;
        }
        return true;
    }

    bool ReadTemperature(float* temperature, float* humidity) {
        const uint8_t command[] = {0xac, 0x33, 0x00};
        if (i2c_master_transmit(temperature_, command, sizeof(command), kTimeoutMs) != ESP_OK)
            return false;
        vTaskDelay(pdMS_TO_TICKS(100));
        uint8_t data[7];
        if (i2c_master_receive(temperature_, data, sizeof(data), kTimeoutMs) != ESP_OK ||
            (data[0] & 0x80) || !(data[0] & 0x08))
            return false;
        uint8_t crc = 0xff;
        for (int i = 0; i < 6; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
        if (crc != data[6])
            return false;
        const uint32_t raw_humidity =
            (uint32_t(data[1]) << 12) | (uint32_t(data[2]) << 4) | (data[3] >> 4);
        const uint32_t raw_temperature =
            (uint32_t(data[3] & 0x0f) << 16) | (uint32_t(data[4]) << 8) | data[5];
        *humidity = raw_humidity * 100.0f / 1048576;
        *temperature = raw_temperature * 200.0f / 1048576 - 50;
        return *temperature >= -40 && *temperature <= 125;
    }
};
