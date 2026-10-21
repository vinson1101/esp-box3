"""No audio/device access: exercise the board UI with real LVGL and a fake codec."""

import os
from pathlib import Path
import subprocess
import runpy
import tempfile
import unittest

from test_startup_wake_word_hint import definition

ROOT = Path(__file__).resolve().parents[2]


class Box3DisplayTests(unittest.TestCase):
    def compile_run(self, source, includes=(), libraries=()):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "check.cc").write_text(source)
            result = subprocess.run(
                ["c++", "-std=c++17", *includes, str(path / "check.cc"),
                 *libraries, "-o", str(path / "check")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(path / "check")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_codec_preserves_persisted_silent_volume(self):
        start = definition((ROOT / "main/audio/audio_codec.cc").read_text(),
                           "void AudioCodec::Start()")
        self.compile_run(r'''
#include <cassert>
#include <initializer_list>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int saved;
struct Settings { Settings(const char*, bool) {} int GetInt(const char*, int) { return saved; } };
struct AudioCodec { int output_volume_ = 70; void Start(); };
''' + start + r'''
int main() {
    for (int value : {0, 1, 50, 100, -1}) {
        saved = value; AudioCodec codec; codec.Start();
        assert(codec.output_volume_ == (value < 0 ? 10 : value));
    }
}
''')

    @unittest.skipUnless(os.environ.get("LVGL_HOST_LIB"), "set LVGL_HOST_LIB for real LVGL UI checks")
    def test_three_pages_navigation_volume_and_deferred_save(self):
        lvgl = Path(os.environ["LVGL_SOURCE_DIR"])
        library = Path(os.environ["LVGL_HOST_LIB"])
        config = Path(os.environ["LVGL_HOST_CONFIG"])
        with tempfile.TemporaryDirectory() as directory:
            stubs = Path(directory)
            (stubs / "display").mkdir()
            for name in ["application.h", "audio_codec.h", "board.h", "display/lcd_display.h"]:
                (stubs / name).write_text('#include "platform.h"\n')
            (stubs / "platform.h").write_text(r'''
#pragma once
#include <lvgl.h>
#include <functional>
#include <vector>
#define ESP_LOGI(...) ((void)0)
struct AudioCodec {
    int volume = 70, writes = 0;
    int output_volume() { return volume; }
    void SetOutputVolume(int v) { volume = v; ++writes; }
};
struct Board {
    AudioCodec codec;
    static Board& GetInstance() { static Board b; return b; }
    AudioCodec* GetAudioCodec() { return &codec; }
};
struct Application {
    std::vector<std::function<void()>> queued;
    static Application& GetInstance() { static Application a; return a; }
    void Schedule(std::function<void()> f) { queued.push_back(f); }
    void Drain() { auto q = std::move(queued); queued.clear(); for (auto& f : q) f(); }
};
struct Theme { const lv_font_t* font; };
struct SpiLcdDisplay {
    bool setup_ui_called_ = false;
    lv_display_t* display_ = lv_display_get_default();
    int width_ = 320, height_ = 240;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    virtual void SetTheme(Theme* theme) {
        lv_obj_set_style_text_font(lv_screen_active(), theme->font, 0);
    }
    virtual void SetEmotion(const char*) {}
    virtual void SetChatMessage(const char*, const char* text) {
        lv_label_set_text(chat_message_label_, text);
    }
    virtual void SetupUI() {
        setup_ui_called_ = true;
        lv_obj_set_style_text_font(lv_screen_active(), &box3_ui_font, 0);
        container_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(container_, 320, 240);
        chat_message_label_ = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(chat_message_label_, &box3_ui_font, 0);
        lv_label_set_text(chat_message_label_, "existing home");
    }
};
struct DisplayLockGuard { template<class T> explicit DisplayLockGuard(T*) {} };
''')
            source = (ROOT / "scripts/tests/ez_fixtures/box3_display_test.cc").read_text()
            fonts = lvgl.parent / "78__xiaozhi-fonts"
            if not (fonts / "scripts/glyph_provider.py").is_file():
                self.skipTest("real text rendering requires the installed Noto font bundle")
            font_source = runpy.run_path(str(ROOT / "scripts/build_box3_ui_font.py"))["generate"](fonts, source)
            source = font_source + '\n' + source
            source = '#include "' + str(ROOT / "main/boards/espressif/esp32-s3-box-3/box3_sensor_assets.cc") + '"\n' + source
            source = '#include "' + str(ROOT / "main/boards/espressif/esp32-s3-box-3/box3_robot_assets.cc") + '"\n' + source
            self.compile_run(source, ["-I" + str(stubs), "-I" + str(lvgl),
                "-I" + str(ROOT / "main/boards/espressif/esp32-s3-box-3"),
                "-I" + str(ROOT / "main/ez"), "-I" + str(ROOT / "main/ez/device_protocol/include"),
                "-I" + str(ROOT / "scripts/tests/ez_fixtures"),
                '-DBOX3_TEST_FONT="' + str(fonts / "cbin/font_noto_sans_common_20_4.bin") + '"',
                '-DLV_CONF_PATH="' + str(config) + '"', "-DLV_KCONFIG_IGNORE", "-DCONFIG_EZ_DEVICE_GATEWAY=1"], [str(library)])
