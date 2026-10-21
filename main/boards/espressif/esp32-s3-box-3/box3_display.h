#pragma once

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "box3_robot.h"
#include "box3_sensor_state.h"
#include "display/lcd_display.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

LV_FONT_DECLARE(box3_ui_font);
LV_IMAGE_DECLARE(box3_sensor_temp);
LV_IMAGE_DECLARE(box3_sensor_humidity);
LV_IMAGE_DECLARE(box3_sensor_rader_on);
LV_IMAGE_DECLARE(box3_sensor_rader_off);

class Box3Display : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    lv_display_t* lvgl_display() const { return display_; }

    bool SensorPageVisible() const { return sensor_visible_.load(); }

    void SetSensorState(const Box3SensorState& state) {
        DisplayLockGuard lock(this);
        if (!SensorPageVisible())
            return;
        const bool connected = state.temperature_connected || state.radar_connected;
        for (auto panel :
             {climate_card_, radar_card_, ir_card_, sensor_status_, sensor_updated_, sensor_back_})
            lv_obj_set_flag(panel, LV_OBJ_FLAG_HIDDEN, !connected);
        lv_obj_set_flag(sensor_missing_, LV_OBJ_FLAG_HIDDEN, connected);
        lv_label_set_text(sensor_missing_status_,
                          state.initialization_error ? "读取异常" : "未连接");
        lv_label_set_text(sensor_status_, state.initialization_error ? "初始化异常"
                                          : !connected               ? "未连接底座"
                                          : !state.temperature_valid ? "温湿度异常"
                                                                     : "底座已连接");
        if (state.temperature_valid) {
            // Match the factory's whole-degree/percent display; LVGL omits float formatting.
            char text[16];
            std::snprintf(text, sizeof(text), "%.0f", state.temperature);
            lv_label_set_text(temperature_label_, text);
            std::snprintf(text, sizeof(text), "%.0f", state.humidity);
            lv_label_set_text(humidity_label_, text);
        } else {
            lv_label_set_text(temperature_label_, "--");
            lv_label_set_text(humidity_label_, "--");
        }
        const bool motion = state.radar_valid && state.motion;
        lv_image_set_src(radar_image_, motion ? &box3_sensor_rader_on : &box3_sensor_rader_off);
        lv_label_set_text(radar_label_, !state.radar_connected     ? "断开"
                                        : state.radar_initializing ? "自检"
                                        : !state.radar_valid       ? "异常"
                                        : motion                   ? "活动"
                                                                   : "安静");
        lv_obj_set_style_text_color(radar_label_, lv_color_hex(motion ? 0xCE244F : 0x606060), 0);
        if (state.updated_ms) {
            lv_label_set_text_fmt(
                sensor_updated_, "%llu 秒前更新",
                static_cast<unsigned long long>((state.now_ms - state.updated_ms) / 1000));
        } else {
            lv_label_set_text(sensor_updated_, "尚未更新");
        }
    }

    void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        // Asset loading releases the previous font after this call returns.
        robot_.SetFonts(
            lv_obj_get_style_text_font(lv_display_get_screen_active(display_), LV_PART_MAIN));
    }

    void SetEmotion(const char* emotion) override {
        bool handled;
        {
            DisplayLockGuard lock(this);
            handled = robot_.SetEmotion(emotion);
        }
        if (!handled)
            SpiLcdDisplay::SetEmotion(emotion);
    }

    void SetChatMessage(const char* role, const char* content) override {
#if CONFIG_EZ_DEVICE_GATEWAY
        if (container_ && (std::strcmp(role, "user") == 0 || std::strcmp(role, "assistant") == 0)) {
            DisplayLockGuard lock(this);
            robot_.SetDialogue(
                role, content,
                lv_obj_get_style_text_font(lv_display_get_screen_active(display_), LV_PART_MAIN));
            return;
        }
#endif
        SpiLcdDisplay::SetChatMessage(role, content);
        if (std::strcmp(role, "system") == 0 && container_) {
            DisplayLockGuard lock(this);
            robot_.SetDetail(content, lv_obj_get_style_text_font(
                                          lv_display_get_screen_active(display_), LV_PART_MAIN));
        }
    }

    void SetupUI() override {
        if (setup_ui_called_) {
            return;
        }
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        auto screen = lv_display_get_screen_active(display_);
        std::vector<lv_obj_t*> home_objects;
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
            home_objects.push_back(lv_obj_get_child(screen, i));
        }

        tiles_ = lv_tileview_create(screen);
        lv_obj_set_size(tiles_, width_, height_);
        lv_obj_set_style_pad_all(tiles_, 0, 0);
        lv_obj_set_style_border_width(tiles_, 0, 0);
        lv_obj_set_scrollbar_mode(tiles_, LV_SCROLLBAR_MODE_OFF);
        for (int i = 0; i < 3; ++i) {
            pages_[i] = lv_tileview_add_tile(
                tiles_, i, 0, i == 0 ? LV_DIR_RIGHT : (i == 2 ? LV_DIR_LEFT : LV_DIR_HOR));
            lv_obj_set_style_pad_all(pages_[i], 0, 0);
            lv_obj_set_style_border_width(pages_[i], 0, 0);
            lv_obj_remove_flag(pages_[i], LV_OBJ_FLAG_SCROLLABLE);
        }
        for (auto object : home_objects) {
            lv_obj_set_parent(object, pages_[0]);
        }
        lv_obj_set_height(container_, height_);
        robot_.Create(pages_[0], lv_obj_get_style_text_font(screen, LV_PART_MAIN));

        lv_obj_set_style_bg_color(pages_[1], lv_color_hex(0xF2F2F2), 0);
        lv_obj_set_style_bg_opa(pages_[1], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(pages_[1], lv_color_hex(0x282828), 0);
        sensor_status_ = SensorLabel(pages_[1], "探测底座中", 42, 1);
        sensor_updated_ = SensorLabel(pages_[1], "尚未更新", 176, 1);
        lv_obj_set_width(sensor_status_, 130);
        lv_obj_set_width(sensor_updated_, 136);
        lv_label_set_long_mode(sensor_status_, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(sensor_updated_, LV_LABEL_LONG_MODE_DOTS);
        auto back = sensor_back_ = Button(pages_[1], "<", 4, 44, 30, 32);
        lv_obj_set_style_bg_color(back, lv_color_white(), 0);
        lv_obj_set_style_text_color(back, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_style_border_width(back, 1, 0);
        lv_obj_set_style_border_color(back, lv_color_hex(0xD0D0D0), 0);
        lv_obj_add_event_cb(
            back,
            [](lv_event_t* event) {
                auto self = static_cast<Box3Display*>(lv_event_get_user_data(event));
                lv_tileview_set_tile_by_index(self->tiles_, 0, 0, LV_ANIM_OFF);
            },
            LV_EVENT_CLICKED, this);
        climate_card_ = SensorCard(42, 38, 164, 52);
        SensorImage(climate_card_, &box3_sensor_temp, 2, 14);
        SensorImage(climate_card_, &box3_sensor_humidity, 82, 14);
        temperature_label_ = SensorLabel(climate_card_, "--", 27, 12);
        humidity_label_ = SensorLabel(climate_card_, "--", 106, 12);
        SensorLabel(climate_card_, "°C", 62, 12);
        SensorLabel(climate_card_, "%", 142, 12);
        radar_card_ = SensorCard(216, 38, 96, 52);
        radar_image_ = SensorImage(radar_card_, &box3_sensor_rader_off, 4, 10);
        radar_label_ = SensorLabel(radar_card_, "自检", 43, 12);
        ir_card_ = SensorCard(42, 100, 270, 128);
        SensorLabel(ir_card_, "活动感应：前方活动变红", 10, 4);
        auto ir_heading = SensorLabel(ir_card_, "红外学习 · 待启用", 10, 42);
        lv_obj_set_style_text_color(ir_heading, lv_color_hex(0xCE244F), 0);
        SensorLabel(ir_card_, "遥控器对准底座学习开关", 10, 80);
        sensor_missing_ = lv_obj_create(pages_[1]);
        lv_obj_remove_style_all(sensor_missing_);
        lv_obj_set_size(sensor_missing_, width_, height_);
        lv_obj_remove_flag(sensor_missing_, LV_OBJ_FLAG_SCROLLABLE);
        Label(sensor_missing_, "传感器", 12);
        sensor_missing_status_ = Label(sensor_missing_, "等待连接", 72);
        Label(sensor_missing_, "等待连接 SENSOR 底座", 112);
        for (auto panel :
             {climate_card_, radar_card_, ir_card_, sensor_status_, sensor_updated_, sensor_back_})
            lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

        Label(pages_[2], "媒体 / 音量", 8);
        volume_label_ = Label(pages_[2], "--", 52);
        slider_ = lv_slider_create(pages_[2]);
        lv_obj_set_size(slider_, 164, 14);
        lv_obj_set_pos(slider_, 78, 115);
        lv_slider_set_range(slider_, 0, 100);
        // Sliding volume must not also swipe to another page.
        lv_obj_remove_flag(slider_, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        lv_obj_add_event_cb(
            slider_,
            [](lv_event_t* event) {
                auto self = static_cast<Box3Display*>(lv_event_get_user_data(event));
                if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
                    self->SetVolumeLabel(lv_slider_get_value(self->slider_));
                } else if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
                    self->SaveVolume(self->volume_);
                }
            },
            LV_EVENT_ALL, this);
        auto down = Button(pages_[2], "-", 12, 94, 48, 48);
        auto up = Button(pages_[2], "+", 260, 94, 48, 48);
        for (auto button : {down, up}) {
            lv_obj_add_event_cb(
                button,
                [](lv_event_t* event) {
                    auto self = static_cast<Box3Display*>(lv_event_get_user_data(event));
                    auto target = lv_event_get_target_obj(event);
                    const int step = lv_obj_get_x(target) < 100 ? -10 : 10;
                    self->SetVolumeLabel(std::clamp(self->volume_ + step, 0, 100));
                    lv_slider_set_value(self->slider_, self->volume_, LV_ANIM_OFF);
                    self->SaveVolume(self->volume_);
                },
                LV_EVENT_CLICKED, this);
        }
        Label(pages_[2], "本机音量 · 自动保存", 160);

        lv_obj_add_event_cb(
            tiles_,
            [](lv_event_t* event) {
                static_cast<Box3Display*>(lv_event_get_user_data(event))->PageChanged();
            },
            LV_EVENT_VALUE_CHANGED, this);
        lv_tileview_set_tile_by_index(tiles_, 0, 0, LV_ANIM_OFF);
        PageChanged();
        ESP_LOGI("Box3Display", "Three pages ready: home / sensor monitor / volume");
    }

private:
    Box3Robot robot_;
    lv_obj_t* tiles_ = nullptr;
    lv_obj_t* pages_[3] = {};
    lv_obj_t* slider_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    int volume_ = 70;
    std::atomic_bool sensor_visible_{false};
    lv_obj_t* sensor_status_ = nullptr;
    lv_obj_t* temperature_label_ = nullptr;
    lv_obj_t* humidity_label_ = nullptr;
    lv_obj_t* radar_label_ = nullptr;
    lv_obj_t* sensor_updated_ = nullptr;
    lv_obj_t* climate_card_ = nullptr;
    lv_obj_t* radar_card_ = nullptr;
    lv_obj_t* ir_card_ = nullptr;
    lv_obj_t* sensor_missing_ = nullptr;
    lv_obj_t* sensor_missing_status_ = nullptr;
    lv_obj_t* sensor_back_ = nullptr;
    lv_obj_t* radar_image_ = nullptr;

    lv_obj_t* SensorCard(int x, int y, int width, int height) {
        auto card = lv_obj_create(pages_[1]);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, width, height);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(card, 8, 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    }

    static lv_obj_t* SensorImage(lv_obj_t* parent, const lv_image_dsc_t* source, int x, int y) {
        auto image = lv_image_create(parent);
        lv_image_set_src(image, source);
        lv_obj_set_pos(image, x, y);
        return image;
    }

    static lv_obj_t* SensorLabel(lv_obj_t* parent, const char* text, int x, int y) {
        auto label = Label(parent, text, 0);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
        return label;
    }

    static lv_obj_t* Label(lv_obj_t* parent, const char* text, int y) {
        auto label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, &box3_ui_font, 0);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
        return label;
    }

    static lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y, int w, int h) {
        auto button = lv_button_create(parent);
        lv_obj_set_size(button, w, h);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        auto label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &box3_ui_font, 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return button;
    }

    void SetVolumeLabel(int volume) {
        volume_ = volume;
        lv_label_set_text_fmt(volume_label_, "%d%%", volume_);
    }

    void SaveVolume(int volume) {
        Application::GetInstance().Schedule([volume]() {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec && codec->output_volume() != volume) {
                codec->SetOutputVolume(volume);
            }
        });
    }

    void PageChanged() {
        auto active = lv_tileview_get_tile_active(tiles_);
        robot_.SetPageVisible(active == pages_[0]);
        sensor_visible_.store(active == pages_[1]);
        if (active == pages_[2]) {
            Application::GetInstance().Schedule([this]() {
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec) {
                    DisplayLockGuard lock(this);
                    SetVolumeLabel(std::clamp(codec->output_volume(), 0, 100));
                    lv_slider_set_value(slider_, volume_, LV_ANIM_OFF);
                }
            });
        }
    }
};
