#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "audio/wake_words/wake_word_audio_cache.h"
#include "ez_dialogue.h"
#include "pcm_framer.h"
#include "speech_end_window.h"
extern "C" {
#include "audio_playback_queue.h"
#include "device_connection.h"
#include "voice_input.h"
}

class EzDevice {
public:
    static EzDevice& GetInstance() {
        static EzDevice device;
        return device;
    }
    void Initialize();
    void NetworkChanged(bool online);
    void Toggle();
    void Wake();
    void StopCapture();
    void Capture(const std::vector<int16_t>& pcm);
    void VadChanged(bool speaking);
    void Tick();
    void ProjectState();

private:
    void StartCapture();
    void BeginCapture();
    void StartFollowup();
    void BeginFollowup();
    void CancelPlayback();
    void ConnectionEvent(const device_connection_event_t& event);
    bool MediaEvent(const device_connection_media_event_t& event);
    void PlaybackTask();
    void ShowStatus(const char* status, const char* message = "",
                    const char* emotion = "unavailable");
    void BeginDialogue(uint64_t capture);
    void ProjectDialogue();
    EzDialogue dialogue_;
    std::atomic<unsigned> pending_text_events_{0};
    void DiscardPlaybackLocked();
    void RememberDiscarded(uint64_t epoch, uint32_t stream);
    device_identity_t* identity_ = nullptr;
    device_connection_t* connection_ = nullptr;
    voice_input_t* voice_input_ = nullptr;
    audio_playback_queue_t* playback_ = nullptr;
    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    uint64_t capture_id_ = 0;
    PcmFramer framer_;
    SpeechEndWindow end_window_;
    bool capture_stop_pending_ = false;
    bool capture_start_pending_ = false;
    bool capture_start_scheduled_ = false;
    uint64_t capture_start_generation_ = 0;
    bool capture_failed_ = false;
    WakeWordAudioCache followup_cache_;
    bool followup_listening_ = false;
    bool followup_start_pending_ = false;
    bool conversation_active_ = false;
    uint64_t network_epoch_ = 0;
    uint64_t connection_epoch_ = 0;
    bool online_ = false;
    bool network_up_ = false;
    bool connection_started_ = false;
    bool pairing_pending_ = false;
    bool heard_voice_ = false;
    int64_t started_ms_ = 0;
    int64_t playback_wait_ms_ = 0;
    int64_t reply_drained_ms_ = 0;
    uint64_t reply_generation_ = 0;
    bool reply_followup_scheduled_ = false;
    std::string playback_run_, reply_text_run_;
    std::array<std::string, 32> discarded_runs_{};
    size_t discarded_run_index_ = 0;
    struct DiscardedStream {
        uint64_t epoch = 0;
        uint32_t stream = 0;
    };
    std::array<DiscardedStream, 32> discarded_{};
    size_t discarded_index_ = 0;
    std::vector<device_media_playback_start_t> active_outputs_;
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> media_epoch_{0};
    std::string detail_ = "等待 EZ 助手";
};
