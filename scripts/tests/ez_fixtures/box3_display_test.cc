#include "box3_display.h"
#include <src/misc/lv_text_private.h>
#include "cbin_host_font.h"
#include "ez_dialogue.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static uint16_t framebuffer[320 * 240];

static void Render(lv_obj_t* screen, const char* name) {
    lv_obj_invalidate(screen);
    lv_refr_now(nullptr);
    if (const char* directory = std::getenv("BOX3_UI_SNAPSHOTS")) {
        std::ofstream out(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
        out << "P6\n320 240\n255\n";
        for (uint16_t pixel : framebuffer) {
            uint8_t r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
            const char rgb[] = {static_cast<char>((r << 3) | (r >> 2)),
                                static_cast<char>((g << 2) | (g >> 4)),
                                static_cast<char>((b << 3) | (b >> 2))};
            out.write(rgb, 3);
        }
        assert(out.good());
    }
}

static lv_obj_t* FindImage(lv_obj_t* object, const lv_image_dsc_t* image) {
    if (lv_obj_check_type(object, &lv_image_class) && lv_image_get_src(object) == image)
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto result = FindImage(lv_obj_get_child(object, i), image))
            return result;
    return nullptr;
}

static lv_obj_t* Find(lv_obj_t* object, const lv_obj_class_t* type, const char* text = nullptr) {
    if (lv_obj_check_type(object, type)) {
        auto label = type == &lv_button_class ? lv_obj_get_child(object, 0) : object;
        if (!text || std::strcmp(lv_label_get_text(label), text) == 0) {
            return object;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        if (auto found = Find(lv_obj_get_child(object, i), type, text)) {
            return found;
        }
    }
    return nullptr;
}

static void Click(lv_obj_t* root, const char* text) {
    auto button = Find(root, &lv_button_class, text);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(root);
}

static lv_indev_data_t pointer;

static void Swipe(lv_indev_t* input, bool left) {
    pointer.state = LV_INDEV_STATE_PRESSED;
    pointer.point.y = 70;
    for (int i = 0; i <= 10; ++i) {
        pointer.point.x = left ? 270 - i * 24 : 30 + i * 24;
        lv_indev_read(input);
        lv_tick_inc(20);
    }
    pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    for (int i = 0; i < 40; ++i) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
}

int main() {
    lv_init();
    lv_font_glyph_dsc_t glyph;
    for (auto text : {"传感器", "媒体 / 音量", "传感器监测", "等待连接 SENSOR 底座",
                      "本机音量 · 自动保存", "0% 100% - +", "单击 Boot 可打断", "配对码：123456"}) {
        uint32_t offset = 0;
        while (text[offset]) {
            auto point = lv_text_encoded_next(text, &offset);
            assert(lv_font_get_glyph_dsc(&box3_ui_font, &glyph, point, 0) && !glyph.is_placeholder);
        }
    }
    auto lcd = lv_display_create(320, 240);
    static uint16_t pixels[320 * 20];
    lv_display_set_color_format(lcd, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(lcd, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lcd, [](lv_display_t* d, const lv_area_t* area, uint8_t* data) {
        auto pixels = reinterpret_cast<uint16_t*>(data);
        for (int y = area->y1; y <= area->y2; ++y)
            for (int x = area->x1; x <= area->x2; ++x)
                framebuffer[y * 320 + x] = *pixels++;
        lv_display_flush_ready(d);
    });
    auto input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, [](lv_indev_t*, lv_indev_data_t* data) { *data = pointer; });
    Box3Display display;
    display.SetupUI();
    auto screen = lv_screen_active();
    lv_obj_update_layout(screen);
    auto tiles = Find(screen, &lv_tileview_class);
    assert(tiles && lv_obj_get_child_count(tiles) == 3);
    assert(lv_obj_get_child_count(screen) == 1);  // No persistent navigation bar.
    assert(lv_obj_get_width(tiles) == 320 && lv_obj_get_height(tiles) == 240);
    for (uint32_t i = 0; i < 3; ++i) {
        assert(lv_obj_get_height(lv_obj_get_child(tiles, i)) == 240);
    }
    assert(Find(lv_obj_get_child(tiles, 0), &lv_label_class, "existing home"));
    assert(Find(lv_obj_get_child(tiles, 1), &lv_label_class, "尚未更新"));
    assert(!display.SensorPageVisible());
    const auto count = lv_obj_get_child_count(screen);
    display.SetupUI();
    assert(lv_obj_get_child_count(screen) == count);

    display.SetChatMessage("system", "Hi, ESP");
    display.SetEmotion("neutral");
    auto pixel = box3_robot_body.data + (60 * 119 + 60) * 4;
    assert(pixel[3] == 255 && pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200);
    auto body = FindImage(screen, &box3_robot_body);
    auto wake_heading = Find(lv_obj_get_parent(body), &lv_label_class, "Hi, ESP");
    assert(wake_heading && !lv_obj_is_visible(wake_heading));
    display.SetChatMessage("system", "Hi, ESP · EZ 已连接");
    auto connected = Find(lv_obj_get_parent(body), &lv_label_class, "Hi, ESP · EZ 已连接");
    assert(connected && lv_obj_is_visible(connected));
    assert(!lv_obj_is_visible(wake_heading));
    Render(screen, "idle-single-line");
    Render(screen, "sleep");
    assert(body && lv_obj_is_visible(body));
    int y = lv_obj_get_y(body);
    for (int i = 0; i < 10; ++i) {
        lv_tick_inc(50);
        lv_timer_handler();
    }
    assert(lv_obj_get_y(body) != y);
    display.SetEmotion("listening");
    Render(screen, "listen");
    auto eye_screen = FindImage(screen, &box3_robot_body_eye_screen);
    const int x = lv_obj_get_x(eye_screen);
    for (int i = 0; i < 4; ++i) {
        lv_tick_inc(50);
        lv_timer_handler();
    }
    assert(lv_obj_get_x(eye_screen) != x);
    Render(screen, "listen-moving");
    bool blinked = false;
    for (int i = 0; i < 40; ++i) {
        lv_tick_inc(50);
        lv_timer_handler();
        if (FindImage(screen, &box3_robot_listen_body_eyes_2)) {
            blinked = true;
            Render(screen, "listen-blink");
        }
    }
    assert(blinked);
    display.SetEmotion("thinking");
    Render(screen, "thinking");
    assert(Find(screen, &lv_label_class, "Thinking"));
    display.SetChatMessage("system", "单击 Boot 可打断");
    display.SetEmotion("speaking");
    Render(screen, "reply");
    assert(!lv_obj_is_visible(body));
    assert(lv_obj_is_visible(FindImage(screen, &box3_robot_reply_body)));
    for (int i = 0; i < 100; ++i) {
        display.SetEmotion("listening");
        display.SetEmotion("thinking");
        display.SetEmotion("speaking");
    }
    assert(lv_anim_count_running() < 8);
    // Asset loading replaces the font after SetupUI; every explicit style must rebind
    // before the old font owner can be released, without a new chat message.
    CbinHostFont replacement(BOX3_TEST_FONT);
    const auto replacement_font = &replacement.font;
    Theme replacement_theme{replacement_font};
    display.SetTheme(&replacement_theme);
    auto reply_note = Find(lv_obj_get_parent(body), &lv_label_class, "单击 Boot 可打断");
    assert(reply_note);
    assert(lv_obj_get_style_text_font(reply_note, LV_PART_MAIN) == replacement_font);
    display.SetEmotion("neutral");
    display.SetChatMessage("system", "EZ 语音服务不可用，请检查主机");
    assert(lv_obj_is_visible(wake_heading));
    Render(screen, "idle-hint");
    assert(lv_obj_is_visible(body));
    auto hint = Find(lv_obj_get_parent(body), &lv_label_class, "EZ 语音服务不可用，请检查主机");
    assert(hint && lv_obj_is_visible(hint));
    assert(lv_obj_get_height(hint) >= box3_ui_font.line_height);
    display.SetChatMessage("system", "");
    assert(lv_obj_is_visible(wake_heading));
    for (auto text : {"Wi-Fi 已断开", "已联网，正在连接 EZ", "Hi, ESP · EZ 已连接"}) {
        display.SetChatMessage("system", text);
        display.SetEmotion("neutral");
        assert(lv_obj_is_visible(body));
        assert(lv_obj_is_visible(Find(lv_obj_get_parent(body), &lv_label_class, text)));
    }
    Render(screen, "reconnected");
    assert(!lv_obj_is_visible(wake_heading));
    display.SetEmotion("unavailable");
    display.SetChatMessage("system", "配对码：123456");
    Render(screen, "fallback");
    assert(!lv_obj_is_visible(body));
    assert(Find(screen, &lv_label_class, "配对码：123456"));
    display.SetEmotion("neutral");

    EzDialogue dialogue;
    const char* question = "请介绍一下 BOX-3 的语音功能，中文和 English 能正确显示吗？";
    const char* answer =
        "这是 ESP32-S3-BOX-3。设备支持本地唤醒、中文识别和语音播放。\n\n"
        "长文本会自动换行；可以上下滑动查看。左右滑动仍然切换到传感器或音量页面。";
    dialogue.Begin(1);
    dialogue.Register(1, "input-1");
    assert(dialogue.Transcript("input-1", question));
    dialogue.Accepted("input-1", "run-1");
    assert(dialogue.Output("run-1", "output-1", answer));
    display.SetChatMessage("assistant", "第一段\r\n第二段\r第三段\t结束");
    assert(Find(lv_obj_get_parent(body), &lv_label_class, "第一段\n第二段\n第三段  结束"));
    display.SetChatMessage("user", "第一段\r\n第二段");
    assert(Find(lv_obj_get_parent(body), &lv_label_class, "第一段 第二段"));
    display.SetChatMessage("user", dialogue.Question().c_str());
    display.SetChatMessage("assistant", dialogue.Answer().c_str());
    display.SetEmotion("speaking");
    Render(screen, "dialogue-reply");
    auto question_label = Find(screen, &lv_label_class, question);
    auto answer_label = Find(lv_obj_get_parent(body), &lv_label_class, answer);
    assert(question_label && answer_label && lv_obj_is_visible(answer_label));
    assert(lv_label_get_long_mode(question_label) == LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(lv_obj_get_height(question_label) >= box3_ui_font.line_height);
    assert(lv_obj_get_style_text_line_space(answer_label, LV_PART_MAIN) == 5);
    auto reply_content = lv_obj_get_parent(answer_label);
    assert(lv_obj_get_height(answer_label) > lv_obj_get_height(reply_content));
    const int initial_scroll = lv_obj_get_scroll_y(reply_content);
    lv_tick_inc(1100);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(reply_content) > initial_scroll);
    Render(screen, "dialogue-autoscroll");
    lv_obj_send_event(reply_content, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(reply_content, LV_EVENT_RELEASED, nullptr);
    const int manual_pause_scroll = lv_obj_get_scroll_y(reply_content);
    lv_tick_inc(1100);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(reply_content) == manual_pause_scroll);
    lv_tick_inc(2200);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(reply_content) > manual_pause_scroll);
    lv_obj_scroll_to_y(reply_content, 80, LV_ANIM_OFF);
    Render(screen, "dialogue-scrolled");
    assert(lv_obj_get_scroll_y(reply_content) > 0);
    const int reply_scroll = lv_obj_get_scroll_y(reply_content);
    std::string extended_answer = std::string(answer) + "\n补充段落。";
    display.SetChatMessage("assistant", extended_answer.c_str());
    lv_obj_update_layout(screen);
    assert(lv_obj_get_scroll_y(reply_content) == reply_scroll);
    display.SetChatMessage("system", "单击 Boot 可打断");
    assert(std::strcmp(lv_label_get_text(answer_label), extended_answer.c_str()) == 0);
    Swipe(input, true);
    const int hidden_scroll = lv_obj_get_scroll_y(reply_content);
    lv_tick_inc(2200);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(reply_content) == hidden_scroll);
    Swipe(input, false);
    display.SetEmotion("neutral");
    assert(!lv_obj_is_visible(answer_label) && lv_obj_is_visible(body));
    assert(std::strcmp(lv_label_get_text(answer_label), extended_answer.c_str()) == 0);
    // Late text and status refreshes must not cover the idle robot again.
    display.SetChatMessage("assistant", extended_answer.c_str());
    display.SetChatMessage("system", "Hi, ESP · EZ 已连接");
    assert(!lv_obj_is_visible(answer_label) && lv_obj_is_visible(body));
    const int stopped_scroll = lv_obj_get_scroll_y(reply_content);
    lv_tick_inc(2200);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(reply_content) == stopped_scroll);
    Swipe(input, true);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 1));
    assert(display.SensorPageVisible());
    Box3SensorState sample;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "未连接"));
    assert(lv_obj_is_visible(Find(screen, &lv_label_class, "等待连接 SENSOR 底座")));
    Render(screen, "sensor-disconnected");
    sample.initialization_error = true;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "读取异常"));
    sample.initialization_error = false;
    sample.radar_connected = sample.radar_initializing = true;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "自检"));
    sample.radar_initializing = false;
    sample.temperature_connected = sample.temperature_valid = true;
    sample.radar_connected = sample.radar_valid = sample.motion = true;
    sample.temperature = 25.5f;
    sample.humidity = 60.2f;
    sample.updated_ms = sample.now_ms = 1000;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "26"));
    assert(Find(screen, &lv_label_class, "60"));
    assert(Find(screen, &lv_label_class, "活动"));
    assert(FindImage(screen, &box3_sensor_rader_on));
    assert(FindImage(screen, &box3_sensor_temp));
    assert(FindImage(screen, &box3_sensor_humidity));
    assert(!lv_obj_is_visible(Find(screen, &lv_label_class, "等待连接 SENSOR 底座")));
    assert(Find(screen, &lv_label_class, "红外学习 · 待启用"));
    Render(screen, "sensor-active");
    sample.motion = false;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "安静"));
    sample.temperature_valid = sample.radar_valid = false;
    sample.now_ms = 6000;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "温湿度异常"));
    assert(Find(screen, &lv_label_class, "--"));
    assert(Find(screen, &lv_label_class, "5 秒前更新"));
    Render(screen, "sensor-error");
    auto sensor_page = lv_obj_get_child(tiles, 1);
    for (uint32_t i = 0; i < lv_obj_get_child_count(sensor_page); ++i) {
        auto label = lv_obj_get_child(sensor_page, i);
        if (!lv_obj_is_visible(label))
            continue;
        assert(lv_obj_get_x(label) >= 0 && lv_obj_get_x2(label) < 320);
        assert(lv_obj_get_y(label) >= 0 && lv_obj_get_y2(label) < 240);
    }
    Click(screen, "<");
    assert(!display.SensorPageVisible());
    sample.temperature_valid = true;
    display.SetSensorState(sample);
    assert(Find(screen, &lv_label_class, "--")); // No hidden-page refresh.

    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 0));
    display.SetEmotion("listening");
    assert(!lv_obj_is_visible(answer_label) && lv_obj_is_visible(body));
    display.SetChatMessage("user", "");
    display.SetChatMessage("assistant", "");
    display.SetEmotion("neutral");
    assert(lv_obj_is_visible(body));

    auto& codec = Board::GetInstance().codec;
    auto& app = Application::GetInstance();
    Swipe(input, true);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 1));
    Swipe(input, false);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 0));
    codec.volume = 30;
    Swipe(input, true);
    Swipe(input, true);
    app.Drain();
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 2));
    assert(!lv_obj_is_visible(body));
    display.SetEmotion("listening");  // State updates must stay on the off-screen home tile.
    display.SetChatMessage("user", question);
    display.SetChatMessage("assistant", answer);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 2));
    Render(screen, "volume");
    auto slider = Find(screen, &lv_slider_class);
    assert(slider && lv_slider_get_value(slider) == 30);
    assert(!lv_obj_has_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR));
    assert(lv_obj_get_y2(slider) < 240);
    Swipe(input, true);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 2));
    for (int i = 0; i < 12; ++i)
        Click(screen, "+");
    assert(codec.volume == 30);  // I2C/NVS must run on the application task.
    app.Drain();
    assert(codec.volume == 100 && Find(screen, &lv_label_class, "100%"));
    for (int i = 0; i < 12; ++i)
        Click(screen, "-");
    app.Drain();
    assert(codec.volume == 0 && Find(screen, &lv_label_class, "0%"));

    const int writes = codec.writes;
    for (int i = 1; i < 60; ++i) {
        lv_slider_set_value(slider, i, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    assert(app.queued.empty() && codec.writes == writes);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    assert(app.queued.size() == 1);
    app.Drain();
    assert(codec.volume == 59 && codec.writes == writes + 1);
    Swipe(input, false);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 1));
    Swipe(input, false);
    assert(lv_tileview_get_tile_active(tiles) == lv_obj_get_child(tiles, 0));
    Swipe(input, true);
    Swipe(input, true);
    app.Drain();
    assert(lv_slider_get_value(slider) == 59);
    assert(lv_obj_get_child_count(screen) == 1);
    // Root deletion must release the scroll timer before its owner disappears.
    {
        Box3Robot temporary;
        auto parent = lv_obj_create(screen);
        temporary.Create(parent, replacement_font);
        temporary.SetDialogue("assistant", answer, replacement_font);
        temporary.SetEmotion("speaking");
        lv_obj_delete(parent);
    }
    lv_tick_inc(2200);
    lv_timer_handler();
}
