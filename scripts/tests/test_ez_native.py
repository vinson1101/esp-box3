import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EZ = ROOT / "main/ez"
FIXTURES = Path(__file__).parent / "ez_fixtures"


@unittest.skipUnless(shutil.which("cc") and shutil.which("c++"), "host compiler unavailable")
class EzNativeTests(unittest.TestCase):
    def compile_run(self, sources, includes, args=(), cpp=False):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "check")
            command = ["c++" if cpp else "cc", "-std=c++17" if cpp else "-std=c11", "-Wall", "-Wextra", "-Werror"]
            command += ["-I" + str(EZ / path) for path in includes]
            command += [str(path) for path in sources] + ["-o", binary]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reference_protocol_and_recovery(self):
        cases = [
            ([EZ / "device_connection/device_connection_supervisor.c", FIXTURES / "device_connection_supervisor_test.c"],
             ["device_connection/include"]),
            ([EZ / "host_discovery/host_selection.c", FIXTURES / "host_discovery_selection_test.c"],
             ["host_discovery/include", "device_protocol/include"]),
            ([EZ / "wss_transport/wss_frame_codec.c", FIXTURES / "wss_transport_codec_test.c"],
             ["wss_transport/private_include", "wss_transport/include", "device_protocol/include"]),
        ]
        for sources, includes in cases:
            with self.subTest(source=sources[0].name):
                self.compile_run(sources, includes)
        fixture = json.loads((FIXTURES / "pcm-v1.json").read_text())
        self.compile_run([EZ / "device_protocol/device_pcm_frame.c", FIXTURES / "device_pcm_frame_test.c"],
                         ["device_protocol/include"], [fixture["header_hex"], fixture["downlink_header_hex"],
                         fixture["stream_id"], fixture["sequence"], fixture["payload_bytes"], *fixture["invalid_header_hex"]])

    def test_playback_fifo_tail_drain_and_cancel(self):
        cjson = Path(os.environ.get("EZ_CJSON_INCLUDE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
        if not (cjson / "cJSON.h").is_file():
            self.skipTest("Set EZ_CJSON_INCLUDE_DIR to the managed cJSON header directory")
        self.compile_run([EZ / "playback/audio_playback_queue.c", FIXTURES / "audio_playback_queue_test.c"],
                         ["playback", "device_protocol/include", cjson])

    def test_afe_chunks_form_exact_wire_frames_and_pad_only_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "framer.cc"
            source.write_text('''#include "pcm_framer.h"
#include <cassert>
#include <vector>
int main() {
    PcmFramer framer;
    std::vector<int16_t> input(1001), output;
    for (size_t i=0; i<input.size(); ++i) input[i]=i-500;
    auto sink=[&](const int16_t* p) { output.insert(output.end(),p,p+320); return true; };
    assert(framer.Push(input.data(),137,sink));
    assert(output.empty());
    assert(framer.Push(input.data()+137,864,sink));
    assert(output.size()==960 && framer.Frames()==3);
    assert(framer.Flush(sink));
    assert(framer.Frames()==4 && output.size()==1280);
    assert(std::equal(input.begin(),input.end(),output.begin()));
    assert(std::all_of(output.begin()+1001,output.end(),[](int16_t v){return v==0;}));
    assert(framer.Flush(sink) && output.size()==1280);
    PcmFramer failed;
    assert(!failed.Push(input.data(),320,[](const int16_t*){return false;}));
    assert(failed.Frames()==0);
}
''')
            self.compile_run([source], ["."], cpp=True)

    def test_afe_confirmed_silence_ends_without_a_second_tail_window(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "end_window.cc"
            source.write_text(r'''#include "speech_end_window.h"
#include <cassert>
int main() {
    SpeechEndWindow w;
    for(int i=0;i<250;++i) assert(!w.Push(false));
    assert(!w.HeardSpeech());
    for(int i=0;i<500;++i) assert(!w.Push(true));
    assert(w.HeardSpeech());
    assert(w.Push(false)); // AFE already waited for 1000 ms of silence.
    w.Reset(true);
    assert(w.Push(false));
    w.Reset();
    assert(!w.Push(false) && !w.HeardSpeech());
}
''')
            self.compile_run([source], ["."], cpp=True)

    def test_followup_waits_for_speech_preserves_preroll_and_exits_on_silence(self):
        # Exercise the actual handlers with a deterministic clock and wire boundary.
        from test_startup_wake_word_hint import definition

        implementation = (EZ / "ez_device.cc").read_text()
        handlers = "\n".join(definition(implementation, signature) for signature in [
            "void EzDevice::StartCapture()", "void EzDevice::BeginCapture()", "void EzDevice::VadChanged(bool speaking)",
            "void EzDevice::StartFollowup()", "void EzDevice::BeginFollowup()",
            "void EzDevice::Capture(const std::vector<int16_t>& pcm)",
            "void EzDevice::StopCapture()", "void EzDevice::Tick()",
            "void EzDevice::ProjectState()",
        ])
        driver = r'''
#include "pcm_framer.h"
#include "speech_end_window.h"
#include "audio/wake_words/wake_word_audio_cache.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
namespace Lang { namespace Sounds { constexpr int OGG_POPUP = 0; } }
enum class PowerSaveLevel { PERFORMANCE };
enum { kDeviceStateWifiConfiguring, kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
enum { VOICE_INPUT_RESULT_OK, VOICE_INPUT_RESULT_INVALID_STATE };
static int64_t clock_ms = 1000;
static int64_t NowMs() { return clock_ms; }
struct AudioService {
    bool voice = false, wake = true, processor = false, idle = true;
    bool IsVoiceDetected() { return voice; }
    bool IsWakeWordRunning() { return wake; }
    bool IsAudioProcessorRunning() { return processor; }
    bool IsPlaybackIdle() { return idle; }
    void EnableVoiceProcessing(bool on) { processor = on; }
    void PlaySound(int) {}
    void EnableWakeWordDetection(bool on) { wake = on; }
};
static AudioService audio;
static AudioService& Audio() { return audio; }
struct Display {
    std::string emotion;
    void SetEmotion(const char* value) { emotion = value; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board b; return b; }
    Display* GetDisplay() { return &display; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct Application {
    int state = kDeviceStateSpeaking;
    std::vector<std::function<void()>> queued;
    static Application& GetInstance() { static Application a; return a; }
    int GetDeviceState() { return state; }
    void SetDeviceState(int s) { state = s; }
    void Schedule(std::function<void()> f) { queued.push_back(std::move(f)); }
    void RunQueued() { auto q = std::move(queued); queued.clear(); for (auto& f : q) f(); }
};
struct voice_input_status_t {};
static int begins = 0, seals = 0, cancels = 0;
static bool reject = false, frame_failure = false;
static uint64_t sealed_frames = 0;
static std::vector<int16_t> wire;
static void voice_input_get_status(void*, voice_input_status_t*) {}
static int voice_input_begin(void*, uint64_t, uint64_t* id) {
    if (reject) return VOICE_INPUT_RESULT_INVALID_STATE;
    *id = ++begins; return VOICE_INPUT_RESULT_OK;
}
static bool voice_input_capture_frame(void*, uint64_t id, const int16_t* p, size_t n) {
    assert(id && n == 320);
    if (frame_failure) return false;
    wire.insert(wire.end(), p, p+n); return true;
}
static int voice_input_cancel(void*, uint64_t) { ++cancels; return VOICE_INPUT_RESULT_OK; }
static int voice_input_seal(void*, uint64_t, uint64_t frames) {
    ++seals; sealed_frames = frames; return VOICE_INPUT_RESULT_OK;
}
struct EzDevice {
    struct Dialogue { void Clear() {} } dialogue_;
    void ProjectDialogue() {}
    void BeginDialogue(uint64_t) {}
    bool online_ = true, conversation_active_ = true;
    std::atomic<bool> ready_{true};
    std::atomic<uint64_t> media_epoch_{1};
    bool network_up_ = true, pairing_pending_ = false;
    bool followup_listening_ = false, followup_start_pending_ = false;
    bool capture_failed_ = false, heard_voice_ = false;
    uint64_t capture_id_ = 0, connection_epoch_ = 1;
    int64_t started_ms_ = 0;
    std::mutex capture_mutex_;
    WakeWordAudioCache followup_cache_;
    PcmFramer framer_;
    SpeechEndWindow end_window_;
    bool capture_stop_pending_ = false;
    bool capture_start_pending_ = false, capture_start_scheduled_ = false;
    uint64_t capture_start_generation_ = 0;
    void* voice_input_ = nullptr;
    std::string detail_;
    std::string emotion;
    void ShowStatus(const char*, const char*, const char* value = "unavailable") { emotion = value; }
    void StartCapture(); void BeginCapture(); void VadChanged(bool); void CancelPlayback() {}
    void StartFollowup(); void BeginFollowup(); void StopCapture();
    void Capture(const std::vector<int16_t>&); void Tick(); void ProjectState();
};
'''
        driver += handlers + r'''
int main() {
    EzDevice d;
    auto& app = Application::GetInstance();
    assert(d.followup_cache_.Initialize(6400));
    d.StartFollowup();
    assert(app.state == kDeviceStateListening && audio.processor && !audio.wake);
    assert(d.emotion == "listening");
    std::vector<int16_t> silent(10000), speech(320), tail(137, 42);
    for (size_t i=0; i<silent.size(); ++i) silent[i]=i;
    for (size_t i=0; i<speech.size(); ++i) speech[i]=10000+i;
    clock_ms += 300;
    d.Capture(silent);
    assert(begins == 0 && app.queued.empty() && wire.empty());
    audio.voice = true;
    d.Capture(speech);
    assert(begins == 0 && app.queued.size() == 1);
    app.RunQueued();
    assert(begins == 1 && wire.size() == 6400);
    for (size_t i=0; i<wire.size(); ++i) assert(wire[i] == static_cast<int16_t>(3920+i));
    d.Capture(tail);
    std::vector<int16_t> quiet(320);
    // AFE has already confirmed its one-second silence before reporting false.
    audio.voice = false;
    clock_ms += 1000;
    d.Capture(quiet);
    assert(app.queued.size() == 1 && seals == 0);
    const auto awaiting_stop_samples = wire.size();
    d.Capture(quiet);
    assert(app.queued.size() == 1 && wire.size() == awaiting_stop_samples);
    app.RunQueued();
    assert(seals == 1 && cancels == 0 && sealed_frames == 22);
    assert(wire.size() == 7040 && wire[6400] == 42 && wire.back() == 0);
    assert(app.state == kDeviceStateIdle && audio.wake);
    app.state = kDeviceStateSpeaking;
    d.StartFollowup();
    clock_ms += 5000;
    d.Tick();
    assert(begins == 1 && seals == 1 && cancels == 0);
    assert(app.state == kDeviceStateIdle && audio.wake && !audio.processor);
    assert(!d.conversation_active_ && d.followup_cache_.Size() == 0);
    d.conversation_active_ = true; app.state = kDeviceStateSpeaking;
    d.StartFollowup(); clock_ms += 300; audio.voice = true;
    d.Capture(speech); d.StopCapture(); app.RunQueued();
    assert(begins == 1 && app.state == kDeviceStateIdle);
    d.conversation_active_ = true; app.state = kDeviceStateSpeaking;
    d.StartFollowup(); clock_ms += 300; reject = true;
    d.Capture(speech); app.RunQueued();
    assert(begins == 1 && app.state == kDeviceStateIdle && audio.wake && !audio.processor);
    reject = false; d.conversation_active_ = true; app.state = kDeviceStateSpeaking;
    d.StartFollowup(); clock_ms += 300; frame_failure = true;
    d.Capture(speech); app.RunQueued(); d.Tick();
    assert(begins == 2 && cancels == 1 && !d.conversation_active_);
    app.state = kDeviceStateIdle; d.detail_ = "正在识别，等待 EZ 回复";
    d.ProjectState(); assert(d.emotion == "thinking");
    d.detail_ = "EZ 助理正在处理";
    d.ProjectState(); assert(d.emotion == "thinking");
    app.state = kDeviceStateSpeaking;
    d.ProjectState(); assert(d.emotion == "speaking");
    app.state = kDeviceStateIdle; d.detail_ = "Hi, ESP · EZ 已连接";
    d.ProjectState(); assert(d.emotion == "neutral");
    d.detail_ = "未检测到语音，请说 Hi, ESP 重试";
    d.ProjectState(); assert(d.emotion == "neutral");
    d.detail_ = "EZ 语音服务不可用，请检查主机";
    d.ProjectState(); assert(d.emotion == "neutral");
    d.online_ = false; d.detail_ = "已联网，正在连接 EZ";
    d.ProjectState(); assert(d.emotion == "neutral");
    d.network_up_ = false; d.detail_ = "Wi-Fi 已断开";
    d.ProjectState(); assert(d.emotion == "neutral");
    d.pairing_pending_ = true; d.detail_ = "配对码：123456";
    d.ProjectState(); assert(d.emotion == "unavailable");
    d.pairing_pending_ = false; d.online_ = true;
    d.detail_ = "Hi, ESP · EZ 已连接";
    d.ProjectState(); assert(d.emotion == "neutral");
    app.state = kDeviceStateWifiConfiguring;
    d.ProjectState(); assert(Board::GetInstance().display.emotion == "unavailable");
    // A queued end from an old capture must not stop a new one.
    app.state = kDeviceStateListening;
    d.capture_id_ = 123; d.capture_failed_ = false; frame_failure = false;
    d.end_window_.Reset(true); d.framer_ = PcmFramer{}; audio.voice = false;
    for (int i = 0; i < 75; ++i) d.Capture(quiet);
    assert(app.queued.size() == 1);
    d.StopCapture();
    d.capture_id_ = 124; d.end_window_.Reset(true);
    app.state = kDeviceStateListening;
    app.RunQueued();
    assert(d.capture_id_ == 124 && app.state == kDeviceStateListening);
    // First-wake capture also ends on frames, without a Tick.
    EzDevice first;
    app.state = kDeviceStateIdle;
    const auto seals_before = seals, cancels_before = cancels;
    audio.idle = false;
    first.StartCapture();
    assert(!first.capture_id_ && first.capture_start_pending_ && !audio.processor);
    first.StopCapture();
    assert(!first.capture_start_pending_ && app.state == kDeviceStateIdle && audio.wake);
    assert(seals == seals_before && cancels == cancels_before);
    audio.idle = true;
    first.StartCapture();
    assert(first.capture_id_ && app.state == kDeviceStateListening);
    audio.voice = true; first.VadChanged(true);
    for (int i=0; i<20; ++i) first.Capture(speech);
    audio.voice = false;
    for (int i=0; i<75; ++i) first.Capture(quiet);
    app.RunQueued();
    assert(seals == seals_before+1 && app.state == kDeviceStateIdle && audio.wake);
    first.StartCapture(); clock_ms += 5000; first.Tick();
    assert(cancels == cancels_before+1 && app.state == kDeviceStateIdle);
    first.StartCapture(); audio.voice = true;
    for (int i=0; i<100; ++i) first.Capture(speech);
    assert(app.queued.empty() && app.state == kDeviceStateListening);
    clock_ms += 60000; first.Tick();
    assert(seals == seals_before+2 && app.state == kDeviceStateIdle);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "esp_heap_caps.h").write_text(
                "#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
                "inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }\n"
                "inline void heap_caps_free(void* p) { std::free(p); }\n")
            (path / "esp_log.h").write_text("#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n")
            source = path / "followup.cc"
            source.write_text(driver)
            self.compile_run([source, ROOT / "main/audio/wake_words/wake_word_audio_cache.cc"],
                             [".", ROOT / "main", path], cpp=True)


if __name__ == "__main__":
    unittest.main()
