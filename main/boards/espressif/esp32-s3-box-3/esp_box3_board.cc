#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "wifi_board.h"
#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
#include <esp_lcd_touch_gt911.h>
#include <esp_lcd_touch_tt21100.h>
#include <esp_lvgl_port.h>
#include "box3_display.h"
#include "box3_sensor.h"
#endif

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#define TAG "EspBox3Board"

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t[]){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t[]){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t[]){0xD0}, 1, 0},
    {0xC1, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0x02}, 1, 0},
    {0xE0,
     (uint8_t[]){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17,
                 0x0F},
     15, 0},
    {0xE1,
     (uint8_t[]){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38,
                 0x0F},
     15, 0},

    {0xB1, (uint8_t[]){00, 0x1B}, 2, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0xB7, (uint8_t[]){0x06}, 1, 0},

    {0x11, (uint8_t[]){0}, 0x80, 0},
    {0x29, (uint8_t[]){0}, 0x80, 0},

    {0, (uint8_t[]){0}, 0xff, 0},
};

class EspBox3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_6;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_7;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (!IsInWifiConfigMode()) {
                    EnterWifiConfigMode();
                }
            });
        });

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_5;
        io_config.dc_gpio_num = GPIO_NUM_4;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_48;
        panel_config.flags.reset_active_high = 1,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ =
            new Box3Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                            DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
    void InitializeSensorMonitor() {
        if (xTaskCreate(
                [](void* argument) {
                    auto display = static_cast<Box3Display*>(argument);
                    Box3Sensor sensor;
                    for (;;) {
                        if (display->SensorPageVisible())
                            display->SetSensorState(sensor.Read());
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                },
                "box3_sensor", 4096, static_cast<Box3Display*>(display_), 2, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "Sensor monitor task unavailable; voice and display remain available");
        }
    }

    void InitializeTouch() {
        esp_lcd_touch_config_t config = {};
        config.x_max = DISPLAY_WIDTH;
        config.y_max = DISPLAY_HEIGHT;
        config.rst_gpio_num = GPIO_NUM_NC;
        config.int_gpio_num = GPIO_NUM_3;
        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        const char* controller = "GT911";
        if (i2c_master_probe(i2c_bus_, io_config.dev_addr, 100) != ESP_OK) {
            io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
            if (i2c_master_probe(i2c_bus_, io_config.dev_addr, 100) != ESP_OK) {
                io_config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
                if (i2c_master_probe(i2c_bus_, io_config.dev_addr, 100) != ESP_OK) {
                    ESP_LOGE(TAG, "Touch controller not found; display remains available");
                    return;
                }
                controller = "TT21100";
                config.flags.mirror_x = 1;
            }
        }
        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_touch_handle_t touch = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &io));
        esp_err_t result = io_config.dev_addr == ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS
                               ? esp_lcd_touch_new_i2c_tt21100(io, &config, &touch)
                               : esp_lcd_touch_new_i2c_gt911(io, &config, &touch);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Touch initialization failed: %s", esp_err_to_name(result));
            esp_lcd_panel_io_del(io);
            return;
        }
        lvgl_port_touch_cfg_t touch_config = {};
        touch_config.disp = static_cast<Box3Display*>(display_)->lvgl_display();
        touch_config.handle = touch;
        if (!lvgl_port_add_touch(&touch_config)) {
            ESP_LOGE(TAG, "Touch input registration failed");
            esp_lcd_touch_del(touch);
            esp_lcd_panel_io_del(io);
            return;
        }
        ESP_LOGI(TAG, "%s touch registered (320x240)", controller);
    }
#endif

public:
    EspBox3Board() : boot_button_(BOOT_BUTTON_GPIO, false, 3000) {
        InitializeI2c();
        InitializeSpi();
        InitializeIli9341Display();
#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
        InitializeTouch();
        InitializeSensorMonitor();
#endif
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspBox3Board);
