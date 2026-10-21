"""Production EZ callback + input state machine; no hardware or real speech service."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_startup_wake_word_hint import definition

ROOT = Path(__file__).resolve().parents[2]
EZ = ROOT / 'main/ez'


@unittest.skipUnless(shutil.which('cc') and shutil.which('c++'), 'host compilers unavailable')
class EzCancelIsolationTests(unittest.TestCase):
    def test_old_physical_cancel_cannot_end_current_logical_replay(self):
        source = (EZ / 'voice_input/voice_input.c').read_text()
        types = source[source.index('typedef enum {'):source.index('struct voice_input {')]
        structure = definition(source, 'struct voice_input {') + ';'
        core = source[source.index('static void status_publish('):source.index('static void expiry_process(')]
        reject = definition(source, 'voice_input_result_t voice_input_reject(')
        media = definition((EZ / 'ez_device.cc').read_text(), 'bool EzDevice::MediaEvent(')
        branch = definition(media, 'if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED)')
        fixture = r'''
#include "voice_input.h"
#include "utterance_cache.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef void *QueueHandle_t,*EventGroupHandle_t,*TaskHandle_t;
typedef int StaticQueue_t,portMUX_TYPE;
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define VOICE_INPUT_TRANSMISSION_BURST 4U
#define VOICE_INPUT_FRAME_BURST 16U
@TYPES@
@STRUCTURE@
static voice_input_t input;
static unsigned active_stream, sequence, busy, mismatch, stops, cancels, rejected;
static unsigned stop_full, start_full, cancel_full;
static voice_input_control_t controls[16];
static size_t control_count;
static uint64_t monotonic_ms(void) { return 1000; }
static uint32_t random_stream_id(void) { return 42; }
uint8_t *utterance_cache_memory_allocate(size_t n) { return malloc(n); }
void utterance_cache_memory_release(uint8_t *p) { free(p); }
static void event_received(const voice_input_event_t *e,void *context) {
    (void)context;
    if(e->type==VOICE_INPUT_EVENT_REJECTED) ++rejected;
}
device_connection_result_t device_connection_send_listen_cancel(device_connection_t *c,uint64_t e,uint32_t stream) {
    (void)c;(void)e;
    if(cancel_full) { --cancel_full; return DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED; }
    ++cancels; if(active_stream==stream) active_stream=0;
    return DEVICE_CONNECTION_RESULT_OK;
}
device_connection_result_t device_connection_send_listen_start(device_connection_t *c,uint64_t e,const char *id,uint32_t stream) {
    (void)c;(void)e;(void)id;
    if(start_full) { --start_full; return DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED; }
    if(active_stream) ++busy; else { active_stream=stream; sequence=0; }
    return DEVICE_CONNECTION_RESULT_OK;
}
device_connection_result_t device_connection_send_pcm(device_connection_t *c,uint64_t e,uint32_t stream,uint32_t seq,const uint8_t *pcm) {
    (void)c;(void)e;(void)pcm;
    if(active_stream!=stream || sequence!=seq) ++mismatch; else ++sequence;
    return DEVICE_CONNECTION_RESULT_OK;
}
device_connection_result_t device_connection_send_listen_stop(device_connection_t *c,uint64_t e,uint32_t stream,uint32_t last) {
    (void)c;(void)e;
    if(stop_full) { --stop_full; return DEVICE_CONNECTION_RESULT_CAPACITY_EXCEEDED; }
    assert(active_stream==stream && last+1==sequence); active_stream=0; ++stops;
    return DEVICE_CONNECTION_RESULT_OK;
}
@CORE@
static voice_input_result_t control_enqueue(voice_input_t *v,const voice_input_control_t *c) {
    assert(v==&input && control_count<16); controls[control_count++]=*c;
    return VOICE_INPUT_RESULT_OK;
}
@REJECT@
voice_input_t *fixture_input(void) { return &input; }
void fixture_controls(void) {
    for(size_t i=0;i<control_count;++i) control_process(&input,&controls[i]);
    control_count=0;
}
void fixture_reset(unsigned stop_block,unsigned start_block,unsigned cancel_block) {
    if(input.cache) utterance_cache_destroy(input.cache);
    memset(&input,0,sizeof(input)); control_count=0;
    active_stream=1; sequence=8; busy=mismatch=stops=cancels=rejected=0;
    stop_full=stop_block;start_full=start_block;cancel_full=cancel_block;
    input.started=true; input.connection_online=true;input.connection_epoch=2;
    input.stream_connection_epoch=2;input.capture_session_id=11;input.stream_id=1;
    input.output_preference=DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO;
    input.event_callback=event_received;strcpy(input.client_input_id,"input-A");
    assert(utterance_cache_create(&input.cache)==UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_begin(input.cache,input.client_input_id,input.output_preference)==UTTERANCE_CACHE_RESULT_OK);
    int16_t pcm[320]={0};
    for(unsigned i=0;i<8;++i) assert(utterance_cache_append(input.cache,pcm,320)==UTTERANCE_CACHE_RESULT_OK);
    input.cached_frame_count=8;input.expected_frame_count=8;input.seal_requested=true;
    input.live_stream_active=true;input.live_next_sequence=8;
    sealing_complete(&input);
}
void fixture_step(void) { cancel_step(&input);transmission_step(&input); }
int fixture_state(void) { return input.status.state; }
int fixture_host_stream(void) { return (int)active_stream; }
int fixture_rejected(void) { return (int)rejected; }
void fixture_accept(void) {
    voice_input_control_t c={.type=VOICE_INPUT_CONTROL_SEGMENT_ACCEPTED};
    c.value.segment_accepted.connection_epoch=2;c.value.segment_accepted.accepted.stream_id=42;
    strcpy(c.value.segment_accepted.accepted.client_input_id,"input-A");control_process(&input,&c);
}
void fixture_block_cancel(void) { cancel_full=1; }
void fixture_user_cancel(void) {
    input_cancel_current(&input,VOICE_INPUT_EVENT_CANCELLED,VOICE_INPUT_RESULT_OK);
}
void fixture_next_round(void) {
    assert(input.status.state==VOICE_INPUT_STATE_IDLE && active_stream==0);
    voice_input_control_t c={.type=VOICE_INPUT_CONTROL_BEGIN};
    c.value.begin.connection_epoch=2;c.value.begin.capture_session_id=12;c.value.begin.stream_id=99;
    strcpy(c.value.begin.client_input_id,"input-B");control_process(&input,&c);
    voice_input_frame_t f={.capture_session_id=12};frame_process(&input,&f);
    assert(input.status.state==VOICE_INPUT_STATE_RECORDING && active_stream==99);
    input.expected_frame_count=1;input.seal_requested=true;sealing_complete(&input);
    assert(active_stream==0 && busy==0 && mismatch==0);
}
void fixture_healthy(void) { assert(busy==0 && mismatch==0); }
void fixture_destroy(void) { utterance_cache_destroy(input.cache);input.cache=NULL; }
'''.replace('@TYPES@', types).replace('@STRUCTURE@', structure).replace('@CORE@', core).replace('@REJECT@', reject)
        driver = r'''
#include "device_connection.h"
#include "voice_input.h"
#include "ez_dialogue.h"
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
extern "C" {
voice_input_t *fixture_input(void);
void fixture_controls(void),fixture_step(void),fixture_accept(void),fixture_user_cancel(void);
void fixture_next_round(void),fixture_healthy(void),fixture_destroy(void),fixture_block_cancel(void);
void fixture_reset(unsigned,unsigned,unsigned);
int fixture_state(void),fixture_host_stream(void),fixture_rejected(void);
}
enum { kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking };
struct Application {
    int state=kDeviceStateIdle;std::vector<std::function<void()>> tasks;
    static Application& GetInstance() { static Application a;return a; }
    int GetDeviceState() { return state; }
    void Schedule(std::function<void()> f) { tasks.push_back(std::move(f)); }
    void Run() { auto q=std::move(tasks);tasks.clear();for(auto &f:q) f(); }
};
struct Device {
    voice_input_t *voice_input_=fixture_input();uint64_t connection_epoch_=2;
    EzDialogue dialogue_;std::string detail_="正在识别";unsigned projected=0;
    void ProjectDialogue() { ++projected; } void ProjectState() { ++projected; }
    bool Handle(const device_connection_media_event_t& event) { BRANCH return false; }
};
static Device device(void) {
    Device d;d.dialogue_.Begin(11);d.dialogue_.Register(11,"input-A");
    assert(d.dialogue_.Transcript("input-A","保留这句输入"));return d;
}
static void idle(Device &d,const char *reason,const char *id="input-A",uint64_t epoch=2) {
    device_connection_media_event_t e{};e.type=DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED;
    e.connection_epoch=epoch;auto &s=e.value.state_changed;s.state=DEVICE_MEDIA_INTERACTION_STATE_IDLE;
    s.has_client_input_id=true;std::strcpy(s.client_input_id,id);
    s.has_reason=reason!=nullptr;if(reason) std::strcpy(s.reason,reason);
    assert(d.Handle(e));fixture_controls();Application::GetInstance().Run();
}
int main() {
    // Old physical cancel before replay start, during PCM, and after replay stop.
    for(int stage=0;stage<3;++stage) {
        fixture_reset(1,stage==0,0);auto d=device();fixture_step();
        if(stage==2) fixture_step();
        assert(fixture_state()!=VOICE_INPUT_STATE_IDLE);
        idle(d,"cancelled");
        assert(fixture_state()!=VOICE_INPUT_STATE_IDLE && fixture_rejected()==0);
        assert(d.dialogue_.Question()=="保留这句输入" && d.detail_=="正在识别" && d.projected==0);
        for(int i=0;i<3 && fixture_host_stream();++i) fixture_step();
        // Stage 0 had not opened replay yet when the delayed notification arrived.
        if(stage==0) { fixture_step();fixture_step(); }
        assert(fixture_host_stream()==0 && fixture_state()==VOICE_INPUT_STATE_AWAITING_ACCEPTANCE);
        fixture_accept();assert(fixture_state()==VOICE_INPUT_STATE_IDLE);
        fixture_next_round();fixture_healthy();
    }
    // A full cancel queue must retry before starting the replay.
    fixture_reset(1,0,1);auto full=device();fixture_step();
    assert(fixture_host_stream()==1);fixture_step();idle(full,"cancelled");fixture_step();
    assert(fixture_host_stream()==0);fixture_accept();fixture_next_round();
    // Explicit user cancellation clears the current replay, including queued cancellation.
    fixture_reset(1,0,0);auto cancelled=device();fixture_step();fixture_block_cancel();fixture_user_cancel();
    assert(fixture_state()==VOICE_INPUT_STATE_CANCELLING && fixture_host_stream()==42);fixture_step();
    idle(cancelled,"cancelled");assert(fixture_state()==VOICE_INPUT_STATE_IDLE && fixture_host_stream()==0);
    fixture_next_round();
    // Genuine terminal/empty rejection still clears input and dialogue; optional reason stays compatible.
    for(const char *reason:{"no_speech_recognized","input_rejected",static_cast<const char *>(nullptr)}) {
        fixture_reset(1,0,0);auto rejected=device();fixture_step();fixture_step();
        idle(rejected,reason);assert(fixture_state()==VOICE_INPUT_STATE_IDLE && fixture_rejected()==1);
        assert(rejected.dialogue_.Question().empty());fixture_next_round();
    }
    // A stale connection or a different input cannot reject the current replay.
    for(int old=0;old<2;++old) {
        fixture_reset(1,0,0);auto stale=device();fixture_step();
        idle(stale,"input_rejected",old?"other-input":"input-A",old?2:1);
        assert(fixture_state()==VOICE_INPUT_STATE_TRANSMITTING && fixture_rejected()==0);
        fixture_step();fixture_accept();fixture_next_round();
    }
    // A cancelled notification arriving in the following independent round cannot close its PCM stream.
    fixture_reset(1,0,0);auto next=device();fixture_step();fixture_step();fixture_accept();
    fixture_next_round();idle(next,"cancelled");fixture_healthy();fixture_destroy();
}
'''.replace('BRANCH', branch)
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            # Only an opaque cJSON type is needed by protocol declarations; no JSON codec is stubbed/executed.
            (p / 'cJSON.h').write_text('typedef struct cJSON cJSON;\n')
            (p / 'input.c').write_text(fixture)
            (p / 'callback.cc').write_text(driver)
            includes = ['-I' + str(p), '-I' + str(EZ)]
            includes += ['-I' + str(path) for path in EZ.glob('*/include')]
            includes += ['-I' + str(EZ / 'utterance_cache/private_include')]
            for name, path in [('input', p / 'input.c'), ('cache', EZ / 'utterance_cache/utterance_cache.c')]:
                command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *includes,
                           '-c', str(path), '-o', str(p / (name + '.o'))]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *includes,
                       str(p / 'callback.cc'), str(p / 'input.o'), str(p / 'cache.o'), '-o', str(p / 'check')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(p / 'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
