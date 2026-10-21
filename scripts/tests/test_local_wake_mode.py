"""Exercise the actual local-mode handlers without any backend implementation."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_startup_wake_word_hint import definition


ROOT = Path(__file__).resolve().parents[2]


class LocalWakeModeTests(unittest.TestCase):
    def test_wifi_ready_and_repeated_wakes_do_not_require_a_protocol(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++ compiler")
        source = (ROOT / "main/application.cc").read_text()
        connected = definition(source, "void Application::HandleNetworkConnectedEvent()")
        wake = definition(source, "void Application::HandleWakeWordDetectedEvent()")
        driver = r'''
            #include <cassert>
            #include <string>
            #include <vector>
            #define CONFIG_LOCAL_WAKE_ONLY 1
            #define ESP_LOGI(...) ((void)0)
            enum DeviceState { kDeviceStateStarting, kDeviceStateWifiConfiguring,
                               kDeviceStateActivating, kDeviceStateIdle };
            struct Display {
                int updates = 0;
                int notifications = 0;
                std::string last;
                void UpdateStatusBar(bool) { ++updates; }
                void ShowNotification(const std::string& word, int) {
                    ++notifications; last = word;
                }
            };
            struct Board {
                Display display;
                static Board& GetInstance() { static Board b; return b; }
                Display* GetDisplay() { return &display; }
            };
            namespace Lang::Sounds { const std::string OGG_POPUP = "popup"; }
            struct AudioService {
                bool running = false;
                int sounds = 0;
                const std::string word = "Hi,ESP";
                const std::string& GetLastWakeWord() { return word; }
                void PlaySound(const std::string& sound) {
                    assert(sound == Lang::Sounds::OGG_POPUP); ++sounds;
                }
                void EnableWakeWordDetection(bool enable) { running = enable; }
            };
            struct Application {
                DeviceState state = kDeviceStateStarting;
                bool startup_completed_ = false;
                bool pending_startup_wake_word_hint_ = false;
                AudioService audio_service_;
                std::vector<DeviceState> transitions;
                DeviceState GetDeviceState() { return state; }
                bool SetDeviceState(DeviceState target) {
                    if (target == kDeviceStateActivating) {
                        assert(state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring);
                    } else {
                        assert(target == kDeviceStateIdle && state == kDeviceStateActivating);
                    }
                    transitions.push_back(target); state = target; return true;
                }
                void HandleNetworkConnectedEvent();
                void HandleWakeWordDetectedEvent();
            };
        ''' + connected + "\n" + wake + r'''
            int main() {
                for (auto state : {kDeviceStateStarting, kDeviceStateWifiConfiguring}) {
                    Application app;
                    app.state = state;
                    app.HandleNetworkConnectedEvent();
                    assert(app.state == kDeviceStateIdle);
                    assert(app.startup_completed_ && app.pending_startup_wake_word_hint_);
                    assert(app.transitions.size() == 2);
                    app.HandleNetworkConnectedEvent();
                    assert(app.transitions.size() == 2);
                }
                for (auto state : {kDeviceStateStarting, kDeviceStateWifiConfiguring,
                                   kDeviceStateIdle}) {
                    Application app;
                    app.state = state;
                    for (int i = 0; i < 3; ++i) {
                        app.audio_service_.running = false;
                        app.HandleWakeWordDetectedEvent();
                        assert(app.audio_service_.running);
                        assert(app.state == state);
                    }
                    assert(app.audio_service_.sounds == 3);
                    assert(Board::GetInstance().display.last == "Hi,ESP");
                }
            }
        '''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            code = path / "local_wake.cc"
            code.write_text(driver)
            binary = path / "local_wake"
            result = subprocess.run(compiler + ["-std=c++17", str(code), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_long_press_uses_the_app_task_and_does_not_restart_an_active_portal(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++ compiler")
        board = (ROOT / "main/boards/espressif/esp32-s3-box-3/esp_box3_board.cc").read_text()
        buttons = definition(board, "void InitializeButtons()")
        driver = r"""
            #include <cassert>
            #include <functional>
            #include <vector>
            #include <utility>
            enum DeviceState { kDeviceStateStarting, kDeviceStateIdle };
            struct Application {
                DeviceState state = kDeviceStateIdle;
                int chats = 0;
                std::vector<std::function<void()>> queued;
                static Application& GetInstance() { static Application a; return a; }
                void Schedule(std::function<void()>&& f) { queued.push_back(std::move(f)); }
                DeviceState GetDeviceState() { return state; }
                void ToggleChatState() { ++chats; }
                void RunQueued() { auto q = std::move(queued); for (auto& f : q) f(); }
            };
            struct Button {
                std::function<void()> long_press, click;
                void OnLongPress(std::function<void()> f) { long_press = std::move(f); }
                void OnClick(std::function<void()> f) { click = std::move(f); }
            };
            struct Board {
                Button boot_button_;
                bool portal = false;
                int entries = 0;
                bool IsInWifiConfigMode() { return portal; }
                void EnterWifiConfigMode() { ++entries; portal = true; }
                BUTTONS
            };
            int main() {
                Board b;
                b.InitializeButtons();
                b.boot_button_.long_press();
                assert(b.entries == 0);
                Application::GetInstance().RunQueued();
                assert(b.entries == 1 && b.portal);
                b.boot_button_.long_press();
                Application::GetInstance().RunQueued();
                assert(b.entries == 1);
                b.boot_button_.click();
                assert(Application::GetInstance().chats == 1);
            }
        """.replace("BUTTONS", buttons)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            code = path / "local_buttons.cc"
            code.write_text(driver)
            binary = path / "local_buttons"
            result = subprocess.run(compiler + ["-std=c++17", str(code), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
