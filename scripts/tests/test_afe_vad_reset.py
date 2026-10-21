"""Actual AFE controls/output and BOX-3 endpoint window, with no acoustic device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_startup_wake_word_hint import definition

ROOT = Path(__file__).resolve().parents[2]


class AfeVadResetTests(unittest.TestCase):
    def test_new_session_resets_sdk_vad_before_fetching_fresh_audio(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        method = definition(source, 'void AfeAudioEngine::ApplyPendingReset()')
        driver = r'''
#include "speech_end_window.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>
#define TAG "AFE"
template<typename... T> static void Log(const char*, const char*, T...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
static constexpr bool kUseAfeForVoiceProcessing=true;
struct FakeAfe {
    bool speaking=true;
    int resets=0, buffers=0;
    int reset_vad(void*) { speaking=false; ++resets; return 1; }
    int reset_buffer(void*) { assert(!speaking); ++buffers; return 1; }
};
struct AfeAudioEngine {
    FakeAfe sdk, *afe_iface_=&sdk;
    void* afe_data_=this;
    std::atomic<bool> reset_pending_{true};
    std::atomic<uint32_t> control_generation_{0};
    std::mutex input_buffer_mutex_;
    std::vector<int16_t> input_buffer_{1,2,3};
    void ApplyPendingReset();
};
''' + method + r'''
int main() {
    AfeAudioEngine engine;
    for(int i=0;i<100;++i) {
        engine.sdk.speaking=true;
        engine.reset_pending_=true;
        engine.ApplyPendingReset();
        assert(engine.input_buffer_.empty() && !engine.sdk.speaking);
        SpeechEndWindow endpoint;
        // Previous speaker activity must not count as speech in the new input.
        assert(!endpoint.Push(engine.sdk.speaking));
        assert(!endpoint.HeardSpeech());
        engine.sdk.speaking=true;
        assert(!endpoint.Push(engine.sdk.speaking));
        engine.sdk.speaking=false;
        assert(endpoint.Push(engine.sdk.speaking));
        engine.ApplyPendingReset();
        assert(engine.sdk.resets==i+1 && engine.sdk.buffers==i+1);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'check.cc').write_text(driver)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'main/ez'), str(p/'check.cc'), '-o', str(p/'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(p/'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_restart_publishes_current_vad_before_pcm_and_keeps_speech_detection(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        methods = '\n'.join(definition(source, signature) for signature in (
            'void AfeAudioEngine::EnableVoiceProcessing(bool enable)',
            'void AfeAudioEngine::UpdateActiveState()',
            'void AfeAudioEngine::HandleVoiceResult('))
        driver = r'''
#include "speech_end_window.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
using EventBits_t = unsigned;
static constexpr unsigned kWakeWordEnabled=1,kVoiceProcessingEnabled=2,kAfeActive=4;
static constexpr bool kUseAfeForVoiceProcessing=true;
static constexpr int VAD_SPEECH=1,VAD_SILENCE=0;
static void xEventGroupSetBits(unsigned* p,unsigned b){*p|=b;}
static void xEventGroupClearBits(unsigned* p,unsigned b){*p&=~b;}
static unsigned xEventGroupGetBits(unsigned* p){return *p;}
struct afe_fetch_result_t {int vad_state;int16_t* data;int data_size; int16_t* vad_cache=nullptr; int vad_cache_size=0;};
struct AfeAudioEngine {
    unsigned bits=kWakeWordEnabled,*event_group_=&bits;
    void* afe_data_=this;
    bool is_speaking_=false;
    int frame_samples_=320;
    std::atomic<bool> reset_pending_{false},output_reset_pending_{false},afe_control_dirty_{false};
    std::atomic<uint32_t> control_generation_{0};
    std::mutex input_buffer_mutex_;
    std::vector<int16_t> input_buffer_,output_buffer_;
    std::function<void(bool)> vad_state_change_callback_;
    std::function<void(std::vector<int16_t>&&)> output_callback_;
    void EnableVoiceProcessing(bool);void UpdateActiveState();
    void HandleVoiceResult(const afe_fetch_result_t*);
};
''' + methods + r'''
int main(){
    std::atomic<bool> voice_detected{false};
    AfeAudioEngine engine;SpeechEndWindow window;
    unsigned closes=0,pcm_frames=0,silent_frames=0,notifications=0;
    engine.vad_state_change_callback_=[&](bool b){voice_detected=b;++notifications;};
    engine.output_callback_=[&](std::vector<int16_t>&& p){
        assert(p.size()==320);++pcm_frames;
        if(!voice_detected)++silent_frames;
        if(window.Push(voice_detected))++closes;
    };
    int16_t pcm[320]{};
    afe_fetch_result_t frame{VAD_SPEECH,pcm,sizeof(pcm)};
    for(unsigned cycle=0;cycle<100;++cycle){
        frame.vad_state=VAD_SPEECH;engine.HandleVoiceResult(&frame);
        assert(voice_detected);
        engine.EnableVoiceProcessing(false);engine.EnableVoiceProcessing(true);
        window.Reset(true);closes=0;silent_frames=0;
        frame.vad_state=VAD_SILENCE;engine.HandleVoiceResult(&frame);
        assert(!voice_detected && silent_frames==1);
        for(int i=1;i<75;++i)engine.HandleVoiceResult(&frame);
        assert(closes>0 && silent_frames==75);
        // Repeated silence must not flood VAD-change callbacks.
        auto before=notifications;engine.HandleVoiceResult(&frame);
        assert(notifications==before);
        // A new real utterance still sets VAD and cannot be cut while speaking.
        window.Reset();closes=0;frame.vad_state=VAD_SPEECH;
        for(int i=0;i<100;++i)engine.HandleVoiceResult(&frame);
        assert(voice_detected && closes==0);
        frame.vad_state=VAD_SILENCE;
        for(int i=0;i<75;++i)engine.HandleVoiceResult(&frame);
        assert(!voice_detected && closes>0);
    }
    assert(pcm_frames>0);
    // Continuous PCM already includes the retrospectively reported VAD onset cache.
    // Exercise real AFE-size chunks across 20ms wire-frame boundaries and resets.
    std::vector<int16_t> output;
    engine.output_callback_=[&](std::vector<int16_t>&& p){ output.insert(output.end(),p.begin(),p.end()); };
    std::vector<int16_t> continuous(7680);
    for(size_t i=0;i<continuous.size();++i)continuous[i]=static_cast<int16_t>(i+1);
    for(int frame_samples:{320,960}) for(bool cached:{false,true}) {
        engine.frame_samples_=frame_samples;
        output.clear();engine.EnableVoiceProcessing(false);engine.EnableVoiceProcessing(true);
        for(size_t i=0;i<15;++i) {
            frame.data=continuous.data()+i*512;frame.data_size=512*sizeof(int16_t);
            frame.vad_state=i>=4?VAD_SPEECH:VAD_SILENCE;
            frame.vad_cache=cached && i==4?continuous.data():nullptr;
            frame.vad_cache_size=cached && i==4?2048*sizeof(int16_t):0;
            engine.HandleVoiceResult(&frame);
        }
        assert(output==continuous); // Every sample, including the first word, exactly once.
        assert(engine.output_buffer_.empty() && voice_detected);
    }
    frame.vad_cache=nullptr;frame.vad_cache_size=0;
    // No callback installed: reset and PCM output still work without dereferencing it.
    engine.vad_state_change_callback_={};engine.EnableVoiceProcessing(false);
    engine.EnableVoiceProcessing(true);engine.HandleVoiceResult(&frame);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'check.cc').write_text(driver)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'main/ez'), str(p/'check.cc'), '-o', str(p/'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(p/'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
