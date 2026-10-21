#include "ez_device.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/platform_util.h>
#include <cstring>
#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

static constexpr const char* TAG = "EZDevice";
static int64_t NowMs() { return esp_timer_get_time() / 1000; }
static AudioService& Audio() { return Application::GetInstance().GetAudioService(); }

void EzDevice::Initialize() {
    ESP_ERROR_CHECK(device_identity_open(&identity_) == DEVICE_IDENTITY_RESULT_OK ? ESP_OK
                                                                                  : ESP_FAIL);
    device_connection_config_t config{};
    config.identity_store = identity_;
    device_identity_record_t record{};
    auto loaded = device_identity_load(identity_, &record);
    if (loaded != DEVICE_IDENTITY_RESULT_OK && loaded != DEVICE_IDENTITY_RESULT_NOT_FOUND) {
        mbedtls_platform_zeroize(&record, sizeof(record));
        detail_ = "EZ 身份读取失败，保留原身份";
        ESP_LOGE(TAG, "Identity load failed: %d", loaded);
        return;
    }
    if (loaded == DEVICE_IDENTITY_RESULT_OK && record.state != DEVICE_IDENTITY_STATE_NONE) {
        config.identity.paired = true;
        std::memcpy(config.identity.installation_id, record.host_installation_id,
                    sizeof(config.identity.installation_id));
        std::memcpy(config.identity.certificate_fingerprint, record.host_certificate_fingerprint,
                    sizeof(config.identity.certificate_fingerprint));
    }
    mbedtls_platform_zeroize(&record, sizeof(record));
    config.capabilities.input_pcm16_16k_mono = true;
    config.capabilities.output_pcm16_16k_mono = true;
    config.capabilities.playback_cancel = true;
    config.capabilities.display_status = true;
    config.capabilities.display_transcript = true;
    config.capabilities.output_text = true;
    config.output_preference = DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
    config.client_version = esp_app_get_description()->version;
    config.display_name = "ESP32-S3-BOX-3";
    config.event_callback = [](const device_connection_event_t* e, void* context) {
        if (e->type == DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED ||
            e->type == DEVICE_CONNECTION_EVENT_HOST_SELECTED)
            return;
        auto self = static_cast<EzDevice*>(context);
        // Protocol event contains no PCM; copy it before leaving the Network task.
        Application::GetInstance().Schedule([self, event = *e]() { self->ConnectionEvent(event); });
    };
    config.event_context = this;
    config.media_callback = [](const device_connection_media_event_t* e, void* context) {
        return static_cast<EzDevice*>(context)->MediaEvent(*e);
    };
    config.media_context = this;
    ESP_ERROR_CHECK(device_connection_create(&config, &connection_) == DEVICE_CONNECTION_RESULT_OK
                        ? ESP_OK
                        : ESP_FAIL);
    voice_input_config_t input{};
    input.connection = connection_;
    input.output_preference = DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
    input.event_context = this;
    input.event_callback = [](const voice_input_event_t* e, void* context) {
        ESP_LOGI(TAG, "Input event=%d result=%d epoch=%llu capture=%llu frames=%llu retries=%lu",
                 e->type, e->result, static_cast<unsigned long long>(e->connection_epoch),
                 static_cast<unsigned long long>(e->capture_session_id),
                 static_cast<unsigned long long>(e->frame_count),
                 static_cast<unsigned long>(e->retransmission_count));
        if (e->type == VOICE_INPUT_EVENT_RECORDING_STARTED) {
            auto self = static_cast<EzDevice*>(context);
            Application::GetInstance().Schedule([self, capture = e->capture_session_id,
                                                 epoch = e->connection_epoch,
                                                 id = std::string(e->client_input_id)]() {
                if (epoch == self->connection_epoch_ && self->online_)
                    self->dialogue_.Register(capture, id);
            });
        }
        if (e->type == VOICE_INPUT_EVENT_FAILED || e->type == VOICE_INPUT_EVENT_REJECTED ||
            e->type == VOICE_INPUT_EVENT_EXPIRED || e->type == VOICE_INPUT_EVENT_TOO_LONG) {
            auto self = static_cast<EzDevice*>(context);
            Application::GetInstance().Schedule([self]() {
                self->detail_ = "语音提交未完成，请重试";
                self->StopCapture();
                self->ProjectState();
            });
        }
    };
    ESP_ERROR_CHECK(voice_input_create(&input, &voice_input_) == VOICE_INPUT_RESULT_OK ? ESP_OK
                                                                                       : ESP_FAIL);
    ESP_ERROR_CHECK(audio_playback_queue_create(&playback_) == AUDIO_PLAYBACK_QUEUE_RESULT_OK
                        ? ESP_OK
                        : ESP_FAIL);
    ESP_ERROR_CHECK(followup_cache_.Initialize(6400) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(voice_input_start(voice_input_) == VOICE_INPUT_RESULT_OK ? ESP_OK : ESP_FAIL);
    ready_.store(true);
    ESP_LOGI(TAG, "Firmware version=%s", esp_app_get_description()->version);
    ESP_ERROR_CHECK(xTaskCreate([](void* ctx) { static_cast<EzDevice*>(ctx)->PlaybackTask(); },
                                "ez_playback", 6144, this, 4, nullptr) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}

void EzDevice::ShowStatus(const char* status, const char* message, const char* emotion) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetChatMessage("system", message);
    display->SetEmotion(emotion);
}

void EzDevice::ProjectDialogue() {
    auto display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("user", dialogue_.Question().c_str());
    display->SetChatMessage("assistant", dialogue_.Answer().c_str());
}

void EzDevice::BeginDialogue(uint64_t capture) {
    dialogue_.Begin(capture);
    ProjectDialogue();
}

void EzDevice::NetworkChanged(bool online) {
    network_up_ = online;
    if (!ready_.load())
        return;
    // Keep LAN multicast and real-time audio reception awake while EZ is active.
    if (online)
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    // mDNS requires WifiManager's netif/event loop, which exists after GOT_IP.
    if (!connection_started_) {
        if (!online)
            return;
        ESP_ERROR_CHECK(device_connection_start(connection_) == DEVICE_CONNECTION_RESULT_OK
                            ? ESP_OK
                            : ESP_FAIL);
        connection_started_ = true;
    }
    if (!online) {
        online_ = false;
        conversation_active_ = false;
        StopCapture();
        CancelPlayback();
        detail_ = "Wi-Fi 已断开";
    }
    auto result = device_connection_network_changed(connection_, online, ++network_epoch_);
    if (result != DEVICE_CONNECTION_RESULT_OK)
        ESP_LOGE(TAG, "Network update failed: %d", result);
    ProjectState();
}

void EzDevice::ConnectionEvent(const device_connection_event_t& event) {
    if (event.type == DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED ||
        event.type == DEVICE_CONNECTION_EVENT_HOST_SELECTED)
        return;
    bool online = event.state == DEVICE_CONNECTION_STATE_ONLINE;
    if (!online || event.connection_epoch != connection_epoch_) {
        dialogue_.Clear();
        ProjectDialogue();
    }
    connection_epoch_ = event.connection_epoch;
    media_epoch_.store(connection_epoch_);
    if (ready_.load())
        voice_input_connection_changed(voice_input_, online, connection_epoch_);
    if (online_ && !online) {
        conversation_active_ = false;
        StopCapture();
        CancelPlayback();
    }
    online_ = online;
    pairing_pending_ = false;
    if (online) {
        detail_ = "Hi, ESP · EZ 已连接";
    } else if (event.block_reason == DEVICE_CONNECTION_BLOCK_PROTOCOL) {
        detail_ = "EZ 通信协议异常，请检查主机";
    } else if (event.block_reason == DEVICE_CONNECTION_BLOCK_DUPLICATE) {
        detail_ = "EZ 连接重复，请检查其他终端";
    } else if (event.block_reason != DEVICE_CONNECTION_BLOCK_NONE) {
        detail_ = "EZ 认证受阻，请检查证书或绑定";
    } else if (!event.paired && event.state == DEVICE_CONNECTION_STATE_AUTHENTICATING &&
               event.pairing_state == DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL) {
        char code[DEVICE_PAIRING_CODE_CAPACITY]{};
        if (device_connection_copy_pairing_code(connection_, code) == DEVICE_CONNECTION_RESULT_OK) {
            pairing_pending_ = true;
            detail_ = std::string("配对码：") + code;
            // Serial is a local physical diagnostics channel; do not publish its logs.
            ESP_LOGI(TAG, "Pairing code: %s", code);
        }
    } else {
        detail_ = event.paired ? "已联网，正在连接 EZ" : "已联网，等待 EZ 开启添加设备";
    }
    ESP_LOGI(
        TAG,
        "Connection state=%d paired=%d block=%d failure=%d transport=%d selection=%d epoch=%llu",
        event.state, event.paired, event.block_reason, event.failure, event.transport_result,
        event.selection_result, static_cast<unsigned long long>(connection_epoch_));
    ProjectState();
}

void EzDevice::ProjectState() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateWifiConfiguring) {
        Board::GetInstance().GetDisplay()->SetEmotion("unavailable");
        Audio().EnableVoiceProcessing(false);
        Audio().EnableWakeWordDetection(true);
        return;
    }
    if (state == kDeviceStateListening) {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        ShowStatus(
            "正在听",
            followup_listening_ ? "请继续说；静默后自动待机" : "说完后自动提交；单击 Boot 结束",
            "listening");
        return;
    }
    Audio().EnableVoiceProcessing(false);
    Audio().EnableWakeWordDetection(true);
    if (state == kDeviceStateSpeaking) {
        ShowStatus("正在播放", "单击 Boot 可打断", "speaking");
    } else {
        // Reuse the existing interaction projection; animation must not drive audio state.
        const bool thinking = detail_.rfind("正在识别", 0) == 0 || detail_ == "EZ 助理正在处理";
        ShowStatus(pairing_pending_ ? "请在 EZ 添加设备"
                   : network_up_    ? "Wi-Fi 已连接"
                                    : "等待 Wi-Fi",
                   detail_.c_str(),
                   pairing_pending_ ? "unavailable"
                   : thinking       ? "thinking"
                                    : "neutral");
    }
}

void EzDevice::Toggle() {
    ESP_LOGI(TAG, "Boot toggle state=%d", Application::GetInstance().GetDeviceState());
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening)
        StopCapture();
    else
        StartCapture();
}

void EzDevice::Wake() {
    ESP_LOGI(TAG, "Wake detected online=%d state=%d", online_,
             Application::GetInstance().GetDeviceState());
    if (!online_) {
        Audio().PlaySound(Lang::Sounds::OGG_POPUP);
        Audio().EnableWakeWordDetection(true);
        ProjectState();
        return;
    }
    StartCapture();
}

void EzDevice::StartCapture() {
    auto& app = Application::GetInstance();
    if (!online_ || !ready_.load() || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
        ProjectState();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (capture_id_ || capture_start_pending_)
            return;
    }
    CancelPlayback();
    // Retire the old speaker and AFE session before accepting any new input.
    Audio().EnableVoiceProcessing(false);
    Audio().EnableWakeWordDetection(false);
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        followup_listening_ = followup_start_pending_ = false;
        followup_cache_.Clear();
        capture_start_pending_ = true;
        capture_start_scheduled_ = false;
        ++capture_start_generation_;
    }
    started_ms_ = NowMs();
    app.SetDeviceState(kDeviceStateListening);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    ProjectState();
    ESP_LOGI(TAG, "Capture preparing generation=%llu playback_idle=%d",
             static_cast<unsigned long long>(capture_start_generation_), Audio().IsPlaybackIdle());
    BeginCapture();
}

void EzDevice::BeginCapture() {
    auto& app = Application::GetInstance();
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!capture_start_pending_)
            return;
        capture_start_scheduled_ = false;
        if (!online_ || app.GetDeviceState() != kDeviceStateListening) {
            capture_start_pending_ = false;
            return;
        }
        if (!Audio().IsPlaybackIdle())
            return;
        capture_start_pending_ = false;
        auto result = voice_input_begin(voice_input_, connection_epoch_, &capture_id_);
        if (result != VOICE_INPUT_RESULT_OK) {
            detail_ = "上一段语音仍在提交，请稍候";
            voice_input_status_t status{};
            voice_input_get_status(voice_input_, &status);
            ESP_LOGW(TAG, "Capture begin rejected result=%d input_state=%d online=%d epoch=%llu",
                     result, status.state, status.connection_online,
                     static_cast<unsigned long long>(status.connection_epoch));
            // WakeNet stops on detection; an unsuccessful input must rearm it too.
            app.SetDeviceState(kDeviceStateIdle);
            Audio().EnableWakeWordDetection(true);
            dialogue_.Clear();
            ProjectDialogue();
            ShowStatus("EZ 已连接", detail_.c_str(), "neutral");
            return;
        }
        framer_ = PcmFramer{};
        capture_failed_ = false;
        capture_stop_pending_ = false;
        end_window_.Reset();
    }
    BeginDialogue(capture_id_);
    conversation_active_ = true;
    heard_voice_ = false;
    started_ms_ = NowMs();
    detail_ = "正在录音";
    Audio().EnableVoiceProcessing(true);
    Audio().PlaySound(Lang::Sounds::OGG_POPUP);
    ProjectState();
    ESP_LOGI(TAG, "Capture started");
}

void EzDevice::StartFollowup() {
    if (!online_ || !conversation_active_)
        return;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (capture_id_ || capture_start_pending_)
            return;
        followup_cache_.Clear();
        followup_listening_ = true;
        followup_start_pending_ = false;
        started_ms_ = NowMs();
    }
    heard_voice_ = false;
    Application::GetInstance().SetDeviceState(kDeviceStateListening);
    Audio().EnableWakeWordDetection(false);
    Audio().EnableVoiceProcessing(true);
    ProjectState();
    ESP_LOGI(TAG, "Follow-up listening started; no input sent until speech");
}

void EzDevice::BeginFollowup() {
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!followup_listening_ || !online_)
            return;
        followup_listening_ = followup_start_pending_ = false;
        auto result = voice_input_begin(voice_input_, connection_epoch_, &capture_id_);
        failed = result != VOICE_INPUT_RESULT_OK;
        if (!failed) {
            framer_ = PcmFramer{};
            capture_failed_ = false;
            capture_stop_pending_ = false;
            // The triggering speech is already in the pre-roll cache.
            end_window_.Reset(true);
            int16_t samples[320];
            for (size_t offset = 0, count; (count = followup_cache_.Read(offset, samples, 320));
                 offset += count) {
                if (!framer_.Push(samples, count, [this](const int16_t* frame) {
                        return voice_input_capture_frame(voice_input_, capture_id_, frame, 320);
                    })) {
                    capture_failed_ = true;
                    break;
                }
            }
        }
        followup_cache_.Clear();
    }
    if (failed) {
        conversation_active_ = false;
        dialogue_.Clear();
        ProjectDialogue();
        detail_ = "续听未能开始，请说 Hi, ESP 重试";
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        ProjectState();
        return;
    }
    CancelPlayback();
    BeginDialogue(capture_id_);
    heard_voice_ = true;
    started_ms_ = NowMs();
    ProjectState();
    ESP_LOGI(TAG, "Capture started followup=1");
}

void EzDevice::Capture(const std::vector<int16_t>& pcm) {
    if (!ready_.load())
        return;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (followup_listening_) {
        followup_cache_.Store(pcm.data(), pcm.size());
        // Wait out the speaker tail locally; listen_start would preempt EZ's pending output.
        if (!followup_start_pending_ && NowMs() - started_ms_ >= 200 && Audio().IsVoiceDetected()) {
            followup_start_pending_ = true;
            Application::GetInstance().Schedule([this]() { BeginFollowup(); });
        }
        return;
    }
    if (!capture_id_ || capture_failed_ || capture_stop_pending_)
        return;
    capture_failed_ = !framer_.Push(pcm.data(), pcm.size(), [this](const int16_t* samples) {
        if (!voice_input_capture_frame(voice_input_, capture_id_, samples, 320))
            return false;
        if (!capture_stop_pending_ && end_window_.Push(Audio().IsVoiceDetected())) {
            capture_stop_pending_ = true;
            Application::GetInstance().Schedule([this, id = capture_id_,
                                                 epoch = media_epoch_.load()]() {
                {
                    std::lock_guard<std::mutex> lock(capture_mutex_);
                    if (id != capture_id_ || epoch != connection_epoch_ || !capture_stop_pending_)
                        return;
                    heard_voice_ = end_window_.HeardSpeech();
                    ESP_LOGI(TAG,
                             "Capture endpoint capture=%llu elapsed_ms=%lld frames=%llu "
                             "source=afe_vadnet silence_ms=1000",
                             static_cast<unsigned long long>(capture_id_), NowMs() - started_ms_,
                             static_cast<unsigned long long>(framer_.Frames()));
                }
                StopCapture();
            });
        }
        return true;
    });
}

void EzDevice::StopCapture() {
    uint64_t id = 0, frames = 0;
    bool cancel = false;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        const bool preparing = capture_start_pending_;
        capture_start_pending_ = capture_start_scheduled_ = false;
        ++capture_start_generation_;
        if (followup_listening_) {
            followup_listening_ = followup_start_pending_ = false;
            followup_cache_.Clear();
        } else if (!capture_id_ && !preparing)
            return;
        if (capture_id_ && !capture_failed_) {
            capture_failed_ = !framer_.Flush([this](const int16_t* samples) {
                return voice_input_capture_frame(voice_input_, capture_id_, samples, 320);
            });
        }
        id = capture_id_;
        frames = id ? framer_.Frames() : 0;
        cancel = capture_failed_ || !heard_voice_ || !frames || !online_;
        capture_id_ = 0;
        capture_stop_pending_ = false;
        end_window_.Reset();
    }
    Audio().EnableVoiceProcessing(false);
    if (id && cancel) {
        dialogue_.Clear();
        ProjectDialogue();
    }
    auto result = !id      ? VOICE_INPUT_RESULT_OK
                  : cancel ? voice_input_cancel(voice_input_, id)
                           : voice_input_seal(voice_input_, id, frames);
    if (result != VOICE_INPUT_RESULT_OK) {
        voice_input_cancel(voice_input_, id);
        dialogue_.Clear();
        ProjectDialogue();
        detail_ = "录音提交失败，请重试";
    } else
        detail_ = !id      ? "Hi, ESP · EZ 已连接"
                  : cancel ? "未检测到语音，请说 Hi, ESP 重试"
                           : "正在识别，等待 EZ 回复";
    if (!id || cancel || result != VOICE_INPUT_RESULT_OK)
        conversation_active_ = false;
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    ProjectState();
    ESP_LOGI(TAG, "Capture stopped frames=%llu cancelled=%d local_only=%d",
             static_cast<unsigned long long>(frames), cancel, !id);
}

void EzDevice::VadChanged(bool speaking) {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateListening)
        return;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    ESP_LOGI(TAG, "VAD speaking=%d capture=%llu followup=%d elapsed_ms=%lld", speaking,
             static_cast<unsigned long long>(capture_id_), followup_listening_,
             NowMs() - started_ms_);
    if (speaking)
        heard_voice_ = true;
}

void EzDevice::Tick() {
    static unsigned diagnostic_ticks = 0;
    if (ready_.load() && ++diagnostic_ticks % 10 == 0) {
        voice_input_status_t status{};
        voice_input_get_status(voice_input_, &status);
        ESP_LOGI(TAG, "Audio state=%d wake=%d processor=%d input=%d online=%d drops=%lu",
                 Application::GetInstance().GetDeviceState(), Audio().IsWakeWordRunning(),
                 Audio().IsAudioProcessorRunning(), status.state, online_,
                 static_cast<unsigned long>(status.dropped_capture_frame_count));
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateListening)
        return;
    auto now = NowMs();
    bool followup;
    bool preparing;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        followup = followup_listening_;
        preparing = capture_start_pending_;
    }
    if (preparing) {
        if (now - started_ms_ >= 5000) {
            ESP_LOGW(TAG, "Capture preparation timed out waiting for playback");
            StopCapture();
        }
        return;
    }
    if (followup) {
        if (now - started_ms_ >= 5000)
            StopCapture();
        return;
    }
    bool failed;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        failed = capture_failed_;
        heard_voice_ = heard_voice_ || end_window_.HeardSpeech();
    }
    if (failed || now - started_ms_ >= 60000 || (!heard_voice_ && now - started_ms_ >= 5000)) {
        ESP_LOGI(TAG, "Capture limit failed=%d heard=%d elapsed_ms=%lld", failed, heard_voice_,
                 static_cast<long long>(now - started_ms_));
        StopCapture();
    }
}

void EzDevice::CancelPlayback() {
    if (!ready_.load())
        return;
    std::lock_guard<std::mutex> lock(playback_mutex_);
    DiscardPlaybackLocked();
    if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    }
}

void EzDevice::RememberDiscarded(uint64_t epoch, uint32_t stream) {
    discarded_[discarded_index_++ % discarded_.size()] = {epoch, stream};
}

void EzDevice::DiscardPlaybackLocked() {
    ++reply_generation_;
    reply_followup_scheduled_ = false;
    reply_drained_ms_ = 0;
    auto retire_run = [this](const std::string& run) {
        if (!run.empty() &&
            std::find(discarded_runs_.begin(), discarded_runs_.end(), run) == discarded_runs_.end())
            discarded_runs_[discarded_run_index_++ % discarded_runs_.size()] = run;
    };
    retire_run(playback_run_);
    playback_run_.clear();
    reply_text_run_.clear();
    audio_playback_queue_status_t status{};
    audio_playback_queue_get_status(playback_, &status);
    for (const auto& output : active_outputs_) {
        ESP_LOGI(TAG, "Playback discard epoch=%llu run=%s output=%s stream=%lu",
                 static_cast<unsigned long long>(status.connection_epoch), output.run_id,
                 output.output_id, static_cast<unsigned long>(output.stream_id));
        retire_run(output.run_id);
        Application::GetInstance().Schedule(
            [this, epoch = status.connection_epoch, run = std::string(output.run_id)]() {
                if (epoch == connection_epoch_) {
                    dialogue_.CancelRun(run);
                    ProjectDialogue();
                }
            });
        RememberDiscarded(status.connection_epoch, output.stream_id);
        device_connection_send_playback_cancel(connection_, status.connection_epoch,
                                               output.output_id, output.stream_id);
    }
    active_outputs_.clear();
    audio_playback_queue_clear(playback_);
    Audio().ResetDecoder();
}

bool EzDevice::MediaEvent(const device_connection_media_event_t& event) {
    if (!ready_.load())
        return false;
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_INPUT_SEGMENT_ACCEPTED)
        return voice_input_segment_accept(voice_input_, event.connection_epoch,
                                          &event.value.input_segment_accepted) ==
               VOICE_INPUT_RESULT_OK;
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_INPUT_ACCEPTED) {
        ESP_LOGI(TAG, "Input accepted epoch=%llu input=%s run=%s state=%d",
                 static_cast<unsigned long long>(event.connection_epoch),
                 event.value.input_accepted.client_input_id, event.value.input_accepted.run_id,
                 event.value.input_accepted.queue_state);
        Application::GetInstance().Schedule(
            [this, epoch = event.connection_epoch, accepted = event.value.input_accepted]() {
                if (epoch == connection_epoch_ && online_) {
                    dialogue_.Accepted(accepted.client_input_id, accepted.run_id);
                    if (accepted.queue_state == DEVICE_MEDIA_RUN_STATUS_CANCELLED ||
                        accepted.queue_state == DEVICE_MEDIA_RUN_STATUS_INTERRUPTED ||
                        accepted.queue_state == DEVICE_MEDIA_RUN_STATUS_FAILED) {
                        dialogue_.CancelRun(accepted.run_id);
                        ProjectDialogue();
                    }
                }
            });
        return voice_input_final_accept(voice_input_, event.connection_epoch,
                                        &event.value.input_accepted) == VOICE_INPUT_RESULT_OK;
    }
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_TRANSCRIPT ||
        event.type == DEVICE_CONNECTION_MEDIA_EVENT_TEXT_OUTPUT) {
        if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_TEXT_OUTPUT) {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            const auto& run = event.value.text_output.run_id;
            if (std::find(discarded_runs_.begin(), discarded_runs_.end(), run) !=
                discarded_runs_.end())
                return true;
            // shortcut: this Host sends the final body after speak delivery; revisit if it streams
            // text.
            reply_text_run_ = run;
        }
        if (pending_text_events_.fetch_add(1) >= 4) {
            --pending_text_events_;
            ESP_LOGW(TAG, "Text projection queue full; preserving audio path");
            return true;
        }
        const bool transcript = event.type == DEVICE_CONNECTION_MEDIA_EVENT_TRANSCRIPT;
        std::string text = transcript ? event.value.transcript.text : event.value.text_output.text;
        std::string id =
            transcript ? event.value.transcript.client_input_id : event.value.text_output.output_id;
        std::string run = transcript ? "" : event.value.text_output.run_id;
        Application::GetInstance().Schedule([this, epoch = event.connection_epoch, transcript,
                                             text = std::move(text), id = std::move(id),
                                             run = std::move(run)]() {
            --pending_text_events_;
            if (epoch != connection_epoch_ || !online_)
                return;
            const bool changed =
                transcript ? dialogue_.Transcript(id, text) : dialogue_.Output(run, id, text);
            if (changed)
                ProjectDialogue();
        });
        return true;
    }
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR) {
        ESP_LOGW(TAG, "Host error code=%s recoverable=%d epoch=%llu correlation=%s",
                 event.value.remote_error.code, event.value.remote_error.recoverable,
                 static_cast<unsigned long long>(event.connection_epoch),
                 event.value.remote_error.has_correlation_message_id
                     ? event.value.remote_error.correlation_message_id
                     : "none");
        // The Host may have sent a stream completely while the local speaker still drains it.
        if (event.value.remote_error.recoverable &&
            std::strcmp(event.value.remote_error.code, "playback_not_active") == 0)
            return true;
        bool empty = std::strcmp(event.value.remote_error.code, "asr_empty_transcript") == 0;
        bool conflict = std::strcmp(event.value.remote_error.code, "audio_input_busy") == 0 ||
                        std::strcmp(event.value.remote_error.code, "pcm_stream_mismatch") == 0;
        Application::GetInstance().Schedule([this, empty, conflict,
                                             epoch = event.connection_epoch]() {
            if (epoch != connection_epoch_)
                return;
            conversation_active_ = false;
            if (conflict) {
                std::lock_guard<std::mutex> lock(capture_mutex_);
                capture_failed_ = true;  // Do not seal/retransmit an input the Host has rejected.
            }
            StopCapture();
            dialogue_.Clear();
            ProjectDialogue();
            detail_ = conflict ? "语音流冲突，正在重新连接 EZ"
                      : empty  ? "没有听到内容，请说 Hi, ESP 重试"
                               : "EZ 语音服务返回错误，请检查主机";
            ProjectState();
        });
        return true;
    }
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED) {
        const auto& state = event.value.state_changed;
        ESP_LOGI(TAG, "Media state=%d epoch=%llu run=%s input=%s reason=%s", state.state,
                 static_cast<unsigned long long>(event.connection_epoch),
                 state.has_run_id ? state.run_id : "none",
                 state.has_client_input_id ? state.client_input_id : "none",
                 state.has_reason ? state.reason : "none");
        // A physical-stream cancel can arrive after a replay of the same logical input starts.
        if (state.state == DEVICE_MEDIA_INTERACTION_STATE_IDLE && state.has_reason &&
            std::strcmp(state.reason, "cancelled") == 0)
            return true;
        if (state.state == DEVICE_MEDIA_INTERACTION_STATE_IDLE && state.has_client_input_id)
            voice_input_reject(voice_input_, event.connection_epoch, state.client_input_id);
        Application::GetInstance().Schedule(
            [this, epoch = event.connection_epoch, state = state.state,
             input = state.has_client_input_id ? std::string(state.client_input_id) : ""]() {
                if (epoch != connection_epoch_)
                    return;
                if (state == DEVICE_MEDIA_INTERACTION_STATE_IDLE && !input.empty()) {
                    dialogue_.RejectInput(input);
                    ProjectDialogue();
                }
                auto current = Application::GetInstance().GetDeviceState();
                if (current == kDeviceStateListening || current == kDeviceStateSpeaking)
                    return;
                if (state == DEVICE_MEDIA_INTERACTION_STATE_RECOGNIZING)
                    detail_ = "正在识别";
                else if (state == DEVICE_MEDIA_INTERACTION_STATE_PROCESSING)
                    detail_ = "EZ 助理正在处理";
                else if (state == DEVICE_MEDIA_INTERACTION_STATE_IDLE)
                    detail_ = "Hi, ESP · EZ 已连接";
                else if (state == DEVICE_MEDIA_INTERACTION_STATE_UNAVAILABLE ||
                         state == DEVICE_MEDIA_INTERACTION_STATE_ERROR) {
                    dialogue_.Clear();
                    ProjectDialogue();
                    detail_ = "EZ 语音服务不可用，请检查主机";
                }
                ProjectState();
            });
        return true;
    }
    std::lock_guard<std::mutex> lock(playback_mutex_);
    uint32_t stream = event.type == DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME
                          ? event.value.pcm_frame.stream_id
                      : event.type == DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END
                          ? event.value.playback_end.stream_id
                          : event.value.playback_start.stream_id;
    for (const auto& discarded : discarded_) {
        if (discarded.epoch == event.connection_epoch && discarded.stream == stream)
            return true;
    }
    audio_playback_queue_result_t result = AUDIO_PLAYBACK_QUEUE_RESULT_OK;
    if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_START) {
        ESP_LOGI(TAG, "Playback start epoch=%llu run=%s output=%s stream=%lu samples=%llu",
                 static_cast<unsigned long long>(event.connection_epoch),
                 event.value.playback_start.run_id, event.value.playback_start.output_id,
                 static_cast<unsigned long>(stream),
                 static_cast<unsigned long long>(event.value.playback_start.sample_count));
        if (std::find(discarded_runs_.begin(), discarded_runs_.end(),
                      event.value.playback_start.run_id) != discarded_runs_.end()) {
            RememberDiscarded(event.connection_epoch, stream);
            ESP_LOGI(TAG, "Playback rejected stream=%lu reason=retired_run",
                     static_cast<unsigned long>(stream));
            device_connection_send_playback_cancel(connection_, event.connection_epoch,
                                                   event.value.playback_start.output_id, stream);
            return true;
        }
        bool capturing;
        {
            std::lock_guard<std::mutex> capture_lock(capture_mutex_);
            capturing = capture_id_ != 0 || capture_start_pending_;
            if (!capturing) {
                followup_listening_ = followup_start_pending_ = false;
                followup_cache_.Clear();
            }
        }
        if (capturing) {
            ESP_LOGI(TAG, "Playback rejected stream=%lu reason=capturing",
                     static_cast<unsigned long>(stream));
            RememberDiscarded(event.connection_epoch, stream);
            device_connection_send_playback_cancel(connection_, event.connection_epoch,
                                                   event.value.playback_start.output_id, stream);
            return true;
        }
        result = audio_playback_queue_begin(playback_, event.connection_epoch,
                                            &event.value.playback_start);
        if (result == AUDIO_PLAYBACK_QUEUE_RESULT_OK) {
            playback_run_ = event.value.playback_start.run_id;
            reply_drained_ms_ = 0;
            reply_followup_scheduled_ = false;
            ++reply_generation_;
            active_outputs_.push_back(event.value.playback_start);
            playback_wait_ms_ = NowMs();
            Application::GetInstance().Schedule(
                [this, epoch = event.connection_epoch, reply_generation = reply_generation_]() {
                    {
                        std::lock_guard<std::mutex> lock(playback_mutex_);
                        if (reply_generation != reply_generation_)
                            return;
                    }
                    bool capturing;
                    {
                        std::lock_guard<std::mutex> lock(capture_mutex_);
                        capturing = capture_id_ != 0 || capture_start_pending_;
                    }
                    if (epoch == connection_epoch_ && !capturing)
                        Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
                });
        }
    } else if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME) {
        result =
            audio_playback_queue_push(playback_, event.connection_epoch, &event.value.pcm_frame);
        playback_wait_ms_ = NowMs();
    } else if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END) {
        ESP_LOGI(TAG, "Playback end epoch=%llu output=%s stream=%lu reason=%s",
                 static_cast<unsigned long long>(event.connection_epoch),
                 event.value.playback_end.output_id, static_cast<unsigned long>(stream),
                 event.value.playback_end.reason);
        if (std::strcmp(event.value.playback_end.reason, "completed") == 0) {
            result = audio_playback_queue_end(playback_, event.connection_epoch,
                                              &event.value.playback_end);
        } else {
            DiscardPlaybackLocked();
        }
    }
    if (result != AUDIO_PLAYBACK_QUEUE_RESULT_OK) {
        audio_playback_queue_status_t status{};
        audio_playback_queue_get_status(playback_, &status);
        ESP_LOGW(TAG,
                 "Playback ingress type=%d result=%d stream=%lu head=%lu streams=%u frames=%u "
                 "received=%llu consumed=%llu",
                 event.type, result, static_cast<unsigned long>(stream),
                 static_cast<unsigned long>(status.stream_id),
                 static_cast<unsigned>(status.stream_count),
                 static_cast<unsigned>(status.total_queued_frame_count),
                 static_cast<unsigned long long>(status.received_frame_count),
                 static_cast<unsigned long long>(status.consumed_sample_count));
    }
    return result == AUDIO_PLAYBACK_QUEUE_RESULT_OK;
}

void EzDevice::PlaybackTask() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            if (capture_start_pending_ && !capture_start_scheduled_ && Audio().IsPlaybackIdle()) {
                capture_start_scheduled_ = true;
                Application::GetInstance().Schedule(
                    [this, generation = capture_start_generation_]() {
                        {
                            std::lock_guard<std::mutex> lock(capture_mutex_);
                            if (generation != capture_start_generation_)
                                return;
                        }
                        BeginCapture();
                    });
            }
        }
        int16_t samples[320];
        size_t count = 0;
        uint32_t generation = 0;
        bool completed = false;
        bool stalled = false;
        uint64_t reply_generation = 0;
        {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            generation = Audio().PlaybackGeneration();
            if (audio_playback_queue_head_ready(playback_, 5)) {
                audio_playback_queue_pop(playback_, samples, &count);
            }
            if (!count && audio_playback_queue_head_drained(playback_) &&
                Audio().IsPlaybackIdle()) {
                audio_playback_queue_complete_head(playback_);
                if (!active_outputs_.empty())
                    active_outputs_.erase(active_outputs_.begin());
            }
            audio_playback_queue_status_t status{};
            audio_playback_queue_get_status(playback_, &status);
            if (!count && !status.stream_count && !playback_run_.empty() &&
                Audio().IsPlaybackIdle()) {
                if (!reply_drained_ms_)
                    reply_drained_ms_ = NowMs();
                // A segment end is not a reply end; also allow queued starts to cross the task
                // boundary.
                if (reply_text_run_ == playback_run_ && NowMs() - reply_drained_ms_ >= 500 &&
                    !reply_followup_scheduled_) {
                    ESP_LOGI(TAG, "Reply drained run=%s generation=%llu", playback_run_.c_str(),
                             static_cast<unsigned long long>(reply_generation_));
                    completed = reply_followup_scheduled_ = true;
                }
            }
            if (!count && !active_outputs_.empty() && NowMs() - playback_wait_ms_ > 15000) {
                DiscardPlaybackLocked();
                completed = true;
                stalled = true;
                ESP_LOGW(TAG,
                         "Playback stalled stream=%lu frames=%u received=%llu consumed=%llu "
                         "wait_ms=%lld",
                         static_cast<unsigned long>(status.stream_id),
                         static_cast<unsigned>(status.total_queued_frame_count),
                         static_cast<unsigned long long>(status.received_frame_count),
                         static_cast<unsigned long long>(status.consumed_sample_count),
                         NowMs() - playback_wait_ms_);
            }
            reply_generation = reply_generation_;
        }
        if (count) {
            if (!Audio().PushPcmToPlaybackQueue(std::vector<int16_t>(samples, samples + count),
                                                generation))
                ESP_LOGW(TAG, "Playback output rejected generation=%lu current=%lu samples=%u",
                         static_cast<unsigned long>(generation),
                         static_cast<unsigned long>(Audio().PlaybackGeneration()),
                         static_cast<unsigned>(count));
        } else {
            if (completed)
                Application::GetInstance().Schedule(
                    [this, epoch = media_epoch_.load(), stalled, reply_generation]() {
                        {
                            std::lock_guard<std::mutex> lock(playback_mutex_);
                            audio_playback_queue_status_t status{};
                            audio_playback_queue_get_status(playback_, &status);
                            if (status.stream_count || reply_generation != reply_generation_)
                                return;
                        }
                        if (epoch == connection_epoch_ &&
                            Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
                            if (!stalled && online_ && conversation_active_) {
                                StartFollowup();
                            } else {
                                conversation_active_ = false;
                                detail_ = "Hi, ESP · EZ 已连接";
                                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                            }
                        }
                    });
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
