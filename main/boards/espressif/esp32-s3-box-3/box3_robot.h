#pragma once

#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>

LV_IMAGE_DECLARE(box3_robot_setup_bg);
LV_IMAGE_DECLARE(box3_robot_body);
LV_IMAGE_DECLARE(box3_robot_body_shadow);
LV_IMAGE_DECLARE(box3_robot_body_eye_screen);
LV_IMAGE_DECLARE(box3_robot_sleep_body_eyes);
LV_IMAGE_DECLARE(box3_robot_sleep_big_z);
LV_IMAGE_DECLARE(box3_robot_sleep_small_z);
LV_IMAGE_DECLARE(box3_robot_listen_back_glow);
LV_IMAGE_DECLARE(box3_robot_listen_body_eyes_1);
LV_IMAGE_DECLARE(box3_robot_listen_body_eyes_2);
LV_IMAGE_DECLARE(box3_robot_reply_body);
LV_IMAGE_DECLARE(box3_robot_reply_small_shadow);
LV_IMAGE_DECLARE(box3_robot_reply_chatgpt_bg);

// The original demo's artwork/timings, owned by the existing LVGL home tile.
class Box3Robot {
public:
    void Create(lv_obj_t* parent, const lv_font_t* font) {
        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, 320, 240);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        Image(root_, &box3_robot_setup_bg, 0, 0);
        shadow_ = Image(root_, &box3_robot_body_shadow, 114, 159);
        glow_ = Image(root_, &box3_robot_listen_back_glow, 93, 36);
        body_ = Image(root_, &box3_robot_body, 101, 53);
        eye_screen_ = Image(body_, &box3_robot_body_eye_screen, 40, 48);
        eyes_ = Image(eye_screen_, &box3_robot_sleep_body_eyes, 0, 0);
        lv_obj_center(eyes_);
        big_z_ = Image(root_, &box3_robot_sleep_big_z, 234, 80);
        small_z_ = Image(root_, &box3_robot_sleep_small_z, 260, 72);
        state_label_ = Label(root_, &lv_font_montserrat_20, 12, 176, 296, 26);
        detail_label_ = Label(root_, font, 12, 207, 296, 28);
        lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

        reply_ = lv_obj_create(root_);
        lv_obj_remove_style_all(reply_);
        lv_obj_set_size(reply_, 320, 240);
        lv_obj_remove_flag(reply_, LV_OBJ_FLAG_SCROLLABLE);
        Image(reply_, &box3_robot_setup_bg, 0, 0);
        Image(reply_, &box3_robot_reply_chatgpt_bg, 8, 5);
        auto mini = Image(reply_, &box3_robot_reply_body, 241, 0);
        Image(mini, &box3_robot_reply_small_shadow, 10, 43);
        reply_question_ = Label(reply_, font, 20, 0, 205, font->line_height + 2);
        lv_label_set_long_mode(reply_question_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(reply_question_, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(reply_question_, "EZ 助手");
        reply_content_ = lv_obj_create(reply_);
        lv_obj_remove_style_all(reply_content_);
        lv_obj_set_pos(reply_content_, 20, 60);
        lv_obj_set_size(reply_content_, 280, 160);
        lv_obj_set_scroll_dir(reply_content_, LV_DIR_VER);
        lv_obj_remove_flag(reply_content_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_scrollbar_mode(reply_content_, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(reply_content_, 2, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(reply_content_, lv_color_white(), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(reply_content_, LV_OPA_50, LV_PART_SCROLLBAR);
        reply_detail_ = Label(reply_content_, font, 0, 0, 250, LV_SIZE_CONTENT);
        lv_label_set_long_mode(reply_detail_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(reply_detail_, 5, 0);
        lv_obj_set_style_text_align(reply_detail_, LV_TEXT_ALIGN_LEFT, 0);
        scroll_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<Box3Robot*>(lv_timer_get_user_data(timer))->ScrollReply();
            },
            1000, this);
        lv_timer_pause(scroll_timer_);
        lv_obj_add_event_cb(
            root_,
            [](lv_event_t* event) {
                auto self = static_cast<Box3Robot*>(lv_event_get_user_data(event));
                lv_timer_delete(self->scroll_timer_);
                self->scroll_timer_ = nullptr;
                self->root_ = nullptr;
            },
            LV_EVENT_DELETE, this);
        lv_obj_add_event_cb(
            reply_content_,
            [](lv_event_t* event) {
                static_cast<Box3Robot*>(lv_event_get_user_data(event))->manual_scroll_ms_ =
                    lv_tick_get();
            },
            LV_EVENT_PRESSED, this);
        Hide(reply_, true);
        Apply();
    }

    void SetDetail(const char* text, const lv_font_t* font) {
        if (!root_)
            return;
        SetFonts(font);
        lv_label_set_text(detail_label_, text ? text : "");
        if (!reply_has_text_)
            lv_label_set_text(reply_detail_, text ? text : "");
        // Noto's line box is taller than its visible glyphs; avoid vertical auto-scroll.
        lv_obj_set_height(detail_label_, font->line_height + 2);
        lv_obj_set_y(detail_label_, 240 - font->line_height - 2);
        UpdateStateLabelVisibility();
    }

    void SetDialogue(const char* role, const char* text, const lv_font_t* font) {
        if (!root_)
            return;
        SetFonts(font);
        if (std::strcmp(role, "user") == 0) {
            std::string value = TextLines(text && *text ? text : "EZ 助手");
            std::replace(value.begin(), value.end(), '\n', ' ');
            if (std::strcmp(lv_label_get_text(reply_question_), value.c_str()) != 0)
                lv_label_set_text(reply_question_, value.c_str());
        } else if (std::strcmp(role, "assistant") == 0) {
            const std::string value = TextLines(text ? text : "");
            if (value == lv_label_get_text(reply_detail_) && reply_has_text_ == !value.empty())
                return;
            const bool had_text = reply_has_text_;
            reply_has_text_ = !value.empty();
            lv_label_set_text(reply_detail_, value.c_str());
            if (!had_text || !reply_has_text_)
                lv_obj_scroll_to_y(reply_content_, 0, LV_ANIM_OFF);
            Apply();
        }
    }

    bool SetEmotion(const char* emotion) {
        Mode next = std::strcmp(emotion, "neutral") == 0     ? Mode::Sleep
                    : std::strcmp(emotion, "listening") == 0 ? Mode::Listen
                    : std::strcmp(emotion, "thinking") == 0  ? Mode::Think
                    : std::strcmp(emotion, "speaking") == 0  ? Mode::Reply
                                                             : Mode::Hidden;
        if (next != mode_) {
            mode_ = next;
            Apply();
        }
        return next != Mode::Hidden;
    }

    void SetPageVisible(bool visible) {
        if (visible_ != visible) {
            visible_ = visible;
            Apply();
        }
    }

    void SetFonts(const lv_font_t* font) {
        if (!root_)
            return;
        for (auto label : {detail_label_, reply_question_, reply_detail_})
            lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_height(reply_question_, font->line_height + 2);
        lv_obj_set_height(detail_label_, font->line_height + 2);
        lv_obj_set_y(detail_label_, 240 - font->line_height - 2);
    }

private:
    enum class Mode { Hidden, Sleep, Listen, Think, Reply };
    Mode mode_ = Mode::Hidden;
    bool visible_ = true;
    bool reply_has_text_ = false;
    lv_timer_t* scroll_timer_ = nullptr;
    uint32_t manual_scroll_ms_ = 0;
    lv_obj_t *root_ = nullptr, *body_ = nullptr, *eye_screen_ = nullptr, *eyes_ = nullptr;
    lv_obj_t *shadow_ = nullptr, *glow_ = nullptr, *big_z_ = nullptr, *small_z_ = nullptr;
    lv_obj_t *state_label_ = nullptr, *detail_label_ = nullptr, *reply_ = nullptr;
    lv_obj_t* reply_detail_ = nullptr;
    lv_obj_t *reply_question_ = nullptr, *reply_content_ = nullptr;

    void ScrollReply() {
        if (!visible_ || mode_ != Mode::Reply || !reply_has_text_ ||
            lv_obj_has_state(reply_content_, LV_STATE_PRESSED) ||
            lv_obj_is_scrolling(reply_content_) ||
            (manual_scroll_ms_ && lv_tick_elaps(manual_scroll_ms_) < 3000))
            return;
        lv_obj_update_layout(reply_content_);
        const auto offset = lv_obj_get_scroll_y(reply_content_);
        const auto remaining = lv_obj_get_scroll_bottom(reply_content_);
        if (remaining > 0) {
            const auto font = lv_obj_get_style_text_font(reply_detail_, LV_PART_MAIN);
            lv_obj_scroll_to_y(reply_content_,
                               offset + std::min<int>(font->line_height / 2, remaining),
                               LV_ANIM_OFF);
        }
    }

    static std::string TextLines(const char* text) {
        std::string result;
        for (size_t i = 0; text[i]; ++i) {
            if (text[i] == '\r') {
                result += '\n';
                if (text[i + 1] == '\n')
                    ++i;
            } else if (text[i] == '\t') {
                result += "  ";
            } else {
                result += text[i];
            }
        }
        return result;
    }

    static void Hide(lv_obj_t* object, bool hidden) {
        if (hidden)
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }

    void UpdateStateLabelVisibility() {
        const bool has_wake_word =
            std::strncmp(lv_label_get_text(detail_label_), "Hi, ESP", 7) == 0;
        Hide(state_label_, mode_ == Mode::Reply || (mode_ == Mode::Sleep && has_wake_word));
    }

    static lv_obj_t* Image(lv_obj_t* parent, const lv_image_dsc_t* source, int x, int y) {
        auto image = lv_image_create(parent);
        lv_image_set_src(image, source);
        lv_obj_set_pos(image, x, y);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
        return image;
    }

    static lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, int x, int y, int w, int h) {
        auto label = lv_label_create(parent);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_size(label, w, h);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        return label;
    }

    static void Animate(lv_obj_t* object, lv_anim_exec_xcb_t callback, int from, int to,
                        uint32_t duration, uint32_t pause = 0, uint32_t delay = 0,
                        uint32_t playback_pause = 0) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, object);
        lv_anim_set_exec_cb(&animation, callback);
        lv_anim_set_values(&animation, from, to);
        lv_anim_set_duration(&animation, duration);
        lv_anim_set_delay(&animation, delay);
        lv_anim_set_playback_duration(&animation, duration);
        lv_anim_set_playback_delay(&animation, playback_pause);
        lv_anim_set_repeat_delay(&animation, pause);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&animation);
    }

    void Apply() {
        if (!root_)
            return;
        lv_timer_pause(scroll_timer_);
        if (visible_ && mode_ == Mode::Reply && reply_has_text_) {
            lv_timer_reset(scroll_timer_);
            lv_timer_resume(scroll_timer_);
        }
        for (auto object : {body_, eye_screen_, eyes_, big_z_, small_z_})
            lv_anim_delete(object, nullptr);
        Hide(root_, !visible_ || mode_ == Mode::Hidden);
        if (!visible_ || mode_ == Mode::Hidden)
            return;
        const bool sleeping = mode_ == Mode::Sleep;
        const bool replying = mode_ == Mode::Reply || (reply_has_text_ && mode_ == Mode::Think);
        Hide(reply_, !replying);
        for (auto object : {body_, shadow_})
            Hide(object, replying);
        Hide(glow_, sleeping || replying);
        Hide(big_z_, !sleeping || replying);
        Hide(small_z_, !sleeping || replying);
        UpdateStateLabelVisibility();
        Hide(detail_label_, replying);
        lv_obj_set_pos(body_, 101, sleeping ? 53 : 44);
        lv_obj_set_pos(eye_screen_, mode_ == Mode::Think ? 30 : 40, 48);
        lv_image_set_src(eyes_,
                         sleeping ? &box3_robot_sleep_body_eyes : &box3_robot_listen_body_eyes_1);
        lv_obj_center(eyes_);
        lv_label_set_text(state_label_, sleeping                ? "Hi, ESP"
                                        : mode_ == Mode::Listen ? "Listening"
                                                                : "Thinking");
        if (replying)
            return;
        if (sleeping) {
            Animate(
                body_, [](void* p, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t*>(p), y); }, 53,
                33, 1000);
            auto fade = [](void* p, int32_t opacity) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(p), opacity, 0);
            };
            Animate(big_z_, fade, 0, 255, 1000);
            Animate(small_z_, fade, 0, 255, 1000, 0, 500);
        } else if (!replying) {
            Animate(
                eyes_,
                [](void* p, int32_t closed) {
                    auto eye = static_cast<lv_obj_t*>(p);
                    lv_image_set_src(eye, closed > 5 ? &box3_robot_listen_body_eyes_2
                                                     : &box3_robot_listen_body_eyes_1);
                    lv_obj_center(eye);
                },
                0, 10, 100, mode_ == Mode::Think ? 1000 : 2100, mode_ == Mode::Think ? 0 : 1800);
            if (mode_ == Mode::Listen)
                Animate(
                    eye_screen_,
                    [](void* p, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(p), x); }, 40, 20,
                    300, 2000, 0, 2000);
        }
    }
};
