#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define private public
#include "ez_device.h"
#undef private
static std::vector<std::string> diagnostic_logs;
template <typename... Args>
static void Diagnostic(const char*, const char* format, Args... args) {
    char line[2048];
    int n;
    if constexpr (sizeof...(Args) == 0)
        n = std::snprintf(line, sizeof(line), "%s", format);
    else
        n = std::snprintf(line, sizeof(line), format, args...);
    assert(n >= 0 && static_cast<size_t>(n) < sizeof(line));
    diagnostic_logs.emplace_back(line);
}
static bool Logged(const char* text) {
    return std::any_of(
        diagnostic_logs.begin(), diagnostic_logs.end(),
        [text](const std::string& line) { return line.find(text) != std::string::npos; });
}
#define ESP_LOGI(...) Diagnostic(__VA_ARGS__)
#define ESP_LOGW(...) Diagnostic(__VA_ARGS__)
#define TAG "EZDevice"
#define pdMS_TO_TICKS(x) (x)
struct StopStep {};
static void vTaskDelay(int) { throw StopStep{}; }
namespace Lang {
namespace Sounds {
constexpr int OGG_POPUP = 0;
}
}  // namespace Lang
enum class PowerSaveLevel { PERFORMANCE };
enum { kDeviceStateWifiConfiguring, kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
static int64_t clock_ms = 1000;
static int64_t NowMs() { return clock_ms; }
struct AudioService {
    bool idle = true, wake = true, processor = false;
    unsigned generation = 1, played = 0;
    uint32_t PlaybackGeneration() { return generation; }
    bool IsPlaybackIdle() { return idle; }
    bool PushPcmToPlaybackQueue(std::vector<int16_t> pcm, uint32_t g) {
        if (g != generation)
            return false;
        played += pcm.size();
        return true;
    }
    void ResetDecoder() { ++generation; }
    void EnableWakeWordDetection(bool b) { wake = b; }
    void EnableVoiceProcessing(bool b) { processor = b; }
    void PlaySound(int) {}
};
static AudioService audio;
static AudioService& Audio() { return audio; }
struct Application {
    int state = kDeviceStateIdle;
    std::vector<std::function<void()>> tasks;
    static Application& GetInstance() {
        static Application a;
        return a;
    }
    int GetDeviceState() { return state; }
    void SetDeviceState(int s) { state = s; }
    void Schedule(std::function<void()> f) { tasks.push_back(std::move(f)); }
    void Drain() {
        auto pending = std::move(tasks);
        tasks.clear();
        for (auto& f : pending)
            f();
    }
};
struct Board {
    static Board& GetInstance() {
        static Board b;
        return b;
    }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
static unsigned starts = 0, playback_cancels = 0, stops = 0;
extern "C" {
uint8_t* audio_playback_queue_memory_allocate(size_t n) {
    return static_cast<uint8_t*>(std::malloc(n));
}
void audio_playback_queue_memory_release(uint8_t* p) { std::free(p); }
voice_input_result_t voice_input_begin(voice_input_t*, uint64_t, uint64_t* id) {
    *id = ++starts;
    return VOICE_INPUT_RESULT_OK;
}
void voice_input_get_status(voice_input_t*, voice_input_status_t* s) { *s = {}; }
bool voice_input_capture_frame(voice_input_t*, uint64_t, const int16_t*, size_t) { return true; }
voice_input_result_t voice_input_segment_accept(voice_input_t*, uint64_t,
                                                const device_media_input_segment_accepted_t*) {
    return VOICE_INPUT_RESULT_OK;
}
voice_input_result_t voice_input_final_accept(voice_input_t*, uint64_t,
                                              const device_media_input_accepted_t*) {
    return VOICE_INPUT_RESULT_OK;
}
voice_input_result_t voice_input_reject(voice_input_t*, uint64_t, const char*) {
    return VOICE_INPUT_RESULT_OK;
}
device_connection_result_t device_connection_send_playback_cancel(device_connection_t*, uint64_t,
                                                                  const char*, uint32_t) {
    ++playback_cancels;
    return DEVICE_CONNECTION_RESULT_OK;
}
}
void EzDevice::BeginDialogue(uint64_t id) { dialogue_.Begin(id); }
void EzDevice::ProjectDialogue() {}
void EzDevice::ProjectState() {}
void EzDevice::ShowStatus(const char*, const char*, const char*) {}
void EzDevice::StopCapture() {
    ++stops;
    capture_start_pending_ = capture_start_scheduled_ = false;
    ++capture_start_generation_;
    capture_id_ = 0;
    Application::GetInstance().state = kDeviceStateIdle;
}
// PRODUCTION_HANDLERS

static device_connection_media_event_t event(device_connection_media_event_type_t type) {
    device_connection_media_event_t e{};
    e.type = type;
    e.connection_epoch = 2;
    return e;
}
static void start(EzDevice& d, unsigned stream, const char* run = "reply") {
    auto e = event(DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_START);
    std::strcpy(e.value.playback_start.output_id, ("out" + std::to_string(stream)).c_str());
    std::strcpy(e.value.playback_start.run_id, run);
    e.value.playback_start.stream_id = stream;
    e.value.playback_start.sample_count = 320;
    assert(d.MediaEvent(e));
    Application::GetInstance().Drain();
}
static void end(EzDevice& d, unsigned stream) {
    static uint8_t pcm[640]{};
    auto e = event(DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME);
    e.value.pcm_frame = {DEVICE_PCM_DIRECTION_DOWNLINK, stream, 0, pcm, sizeof(pcm)};
    assert(d.MediaEvent(e));
    e = event(DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END);
    std::strcpy(e.value.playback_end.output_id, ("out" + std::to_string(stream)).c_str());
    std::strcpy(e.value.playback_end.reason, "completed");
    e.value.playback_end.stream_id = stream;
    assert(d.MediaEvent(e));
}
static void text(EzDevice& d, const char* run = "reply") {
    auto e = event(DEVICE_CONNECTION_MEDIA_EVENT_TEXT_OUTPUT);
    std::strcpy(e.value.text_output.output_id, "final-text");
    std::strcpy(e.value.text_output.run_id, run);
    e.value.text_output.text = "完整正文";
    assert(d.MediaEvent(e));
    Application::GetInstance().Drain();
}
static void pump(EzDevice& d, bool drain = true) {
    try {
        d.PlaybackTask();
    } catch (const StopStep&) {
    }
    if (drain)
        Application::GetInstance().Drain();
}
static void initialize(EzDevice& d) {
    d.ready_ = true;
    d.online_ = true;
    d.conversation_active_ = true;
    d.connection_epoch_ = 2;
    d.media_epoch_ = 2;
    assert(audio_playback_queue_create(&d.playback_) == AUDIO_PLAYBACK_QUEUE_RESULT_OK);
    assert(d.followup_cache_.Initialize(6400));
    Application::GetInstance().tasks.clear();
    Application::GetInstance().state = kDeviceStateIdle;
}
static void check_interrupt() {
    auto& app = Application::GetInstance();
    EzDevice interrupted;
    initialize(interrupted);
    start(interrupted, 1);
    end(interrupted, 1);
    interrupted.Wake();
    assert(interrupted.capture_id_ && app.state == kDeviceStateListening);
    const auto before = stops;
    auto error = event(DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR);
    std::strcpy(error.value.remote_error.code, "playback_not_active");
    error.value.remote_error.recoverable = true;
    assert(interrupted.MediaEvent(error));
    app.Drain();
    assert(stops == before && interrupted.capture_id_ && app.state == kDeviceStateListening);
    assert(interrupted.conversation_active_);
    assert(Logged("Host error code=playback_not_active recoverable=1 epoch=2"));
}
static void check_reply() {
    auto& app = Application::GetInstance();
    EzDevice reply;
    initialize(reply);
    start(reply, 10);
    end(reply, 10);
    pump(reply);
    // A long inter-segment gap must not start a microphone while the reply is unfinished.
    clock_ms += 10000;
    pump(reply);
    assert(app.state == kDeviceStateSpeaking && !reply.followup_listening_);
    start(reply, 11);
    end(reply, 11);
    text(reply);
    audio.idle = false;
    pump(reply);
    clock_ms += 1000;
    pump(reply);
    assert(app.state == kDeviceStateSpeaking && !reply.followup_listening_);
    audio.idle = true;
    pump(reply);
    clock_ms += 1000;
    pump(reply);
    assert(app.state == kDeviceStateListening && reply.followup_listening_);
    assert(Logged("Reply drained run=reply"));
    assert(Logged("Playback end epoch=2 output=out11 stream=11 reason=completed"));
}
static void check_queued() {
    auto& app = Application::GetInstance();
    // A new late stream arriving before a queued completion must invalidate that completion.
    EzDevice queued;
    initialize(queued);
    start(queued, 20);
    end(queued, 20);
    text(queued);
    pump(queued, false);
    clock_ms += 1000;
    pump(queued, false);
    start(queued, 21);
    assert(app.state == kDeviceStateSpeaking && !queued.followup_listening_);
    // A cancelled reply cannot be revived by an unseen later segment or final text.
    queued.Wake();
    assert(app.state == kDeviceStateListening);
    queued.capture_id_ = 0;
    app.state = kDeviceStateIdle;
    start(queued, 22);
    text(queued);
    assert(app.state == kDeviceStateIdle && queued.active_outputs_.empty());
    start(queued, 23, "new-reply");
    end(queued, 23);
    text(queued, "new-reply");
    pump(queued);
    clock_ms += 1000;
    pump(queued);
    assert(app.state == kDeviceStateListening && queued.followup_listening_);
    // Fatal/provider errors remain visible; this is not a blanket ignore of remote errors.
    auto error = event(DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR);
    std::strcpy(error.value.remote_error.code, "asr_provider_failed");
    assert(queued.MediaEvent(error));
    app.Drain();
    assert(!queued.conversation_active_ && queued.detail_ == "EZ 语音服务返回错误，请检查主机");
    assert(Logged("Playback rejected stream=22 reason=retired_run"));
}
static void check_diagnostics() {
    EzDevice d;
    initialize(d);
    start(d, 30);
    static uint8_t pcm[640]{};
    auto e = event(DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME);
    e.value.pcm_frame = {DEVICE_PCM_DIRECTION_DOWNLINK, 30, 9, pcm, sizeof(pcm)};
    assert(!d.MediaEvent(e));
    assert(Logged("Playback start epoch=2 run=reply output=out30 stream=30 samples=320"));
    assert(Logged("stream=30 head=30 streams=1 frames=0 received=0 consumed=0"));
    text(d);
    assert(!Logged("完整正文"));
}
static void check_drain(bool cancel) {
    auto& app = Application::GetInstance();
    EzDevice d;
    initialize(d);
    start(d, 40);
    // ResetDecoder clears queued PCM, but a speaker write may still be in flight.
    audio.idle = false;
    const auto before = starts;
    d.StartCapture();
    assert(starts == before && d.capture_start_pending_ && !audio.processor && !audio.wake);
    pump(d);
    assert(starts == before);
    // An old run cannot resume while the new capture is preparing.
    start(d, 41);
    assert(d.active_outputs_.empty() && d.capture_start_pending_);
    audio.idle = true;
    pump(d, false);
    if (cancel) {
        d.StopCapture();
        audio.idle = false;
        d.StartCapture();
        app.Drain();
        assert(starts == before && d.capture_start_pending_ && !audio.processor);
        audio.idle = true;
        pump(d);
    } else {
        // Repeated task pumps must queue exactly one start.
        const auto pending = app.tasks.size();
        pump(d, false);
        assert(app.tasks.size() == pending);
        app.Drain();
    }
    assert(starts == before + 1 && d.capture_id_ && !d.capture_start_pending_ && audio.processor);
    assert(app.state == kDeviceStateListening);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    if (std::strcmp(argv[1], "interrupt") == 0)
        check_interrupt();
    else if (std::strcmp(argv[1], "reply") == 0)
        check_reply();
    else if (std::strcmp(argv[1], "queued") == 0)
        check_queued();
    else if (std::strcmp(argv[1], "drain") == 0)
        check_drain(false);
    else if (std::strcmp(argv[1], "cancel_start") == 0)
        check_drain(true);
    else {
        assert(std::strcmp(argv[1], "diagnostics") == 0);
        check_diagnostics();
    }
}
