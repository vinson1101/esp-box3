"""Exercise the production online error branch with the real backoff supervisor."""
from pathlib import Path
import tempfile
import unittest

import test_ez_native as native
from test_ez_native import EZ
from test_startup_wake_word_hint import definition


class EzStreamRecoveryTests(unittest.TestCase):
    compile_run = native.EzNativeTests.compile_run

    def test_stream_conflict_reconnects_but_other_protocol_errors_still_block(self):
        implementation = (EZ / 'device_connection/device_connection.c').read_text()
        handler = definition(implementation, 'static void online_control_handle(')
        branch = definition(handler, '} else if (strcmp(message_type, "error") == 0)')
        body = branch[branch.index('{'):]
        driver = r'''
#include "device_connection_supervisor.h"
#include <assert.h>
#include <string.h>
typedef struct { const char *code; bool recoverable; } device_authentication_error_t;
typedef struct { device_authentication_error_t error; bool valid; } device_control_envelope_t;
typedef struct { device_connection_supervisor_t supervisor; unsigned events; bool revoked; } device_connection_t;
typedef struct {
    int type; uint64_t connection_epoch;
    union { device_authentication_error_t remote_error; } value;
} device_connection_media_event_t;
enum { DEVICE_AUTHENTICATION_RESULT_OK, WSS_TRANSPORT_RESULT_PROTOCOL_ERROR,
       DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR };
static int device_authentication_decode_error(const device_control_envelope_t *e,
                                               device_authentication_error_t *error) {
    *error=e->error; return e->valid ? DEVICE_AUTHENTICATION_RESULT_OK : -1;
}
static bool emit_media_event(device_connection_t *c, const device_connection_media_event_t *e) {
    assert(e->connection_epoch == c->supervisor.connection_epoch); ++c->events; return true;
}
static void clear_credential_and_discover(device_connection_t *c) { c->revoked=true; }
static void record_failure(device_connection_t *c, device_connection_failure_t f, int transport) {
    assert(transport == WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
    assert(device_connection_supervisor_fail(&c->supervisor,c->supervisor.connection_epoch,f,1000,0));
}
static void handle(device_connection_t *connection, const device_control_envelope_t *envelope)
''' + body + r'''
static void online(device_connection_t *c) {
    memset(c,0,sizeof(*c));
    device_connection_supervisor_initialize(&c->supervisor);
    device_connection_supervisor_got_ip(&c->supervisor,0);
    uint64_t epoch;
    assert(device_connection_supervisor_connect_discovered(&c->supervisor,&epoch));
    assert(device_connection_supervisor_transport_ready(&c->supervisor,epoch));
    assert(device_connection_supervisor_authenticated(&c->supervisor,epoch,1));
}
int main(void) {
    const char *conflicts[]={"audio_input_busy","pcm_stream_mismatch"};
    for(unsigned i=0;i<2;++i) for(unsigned recoverable=0;recoverable<2;++recoverable) {
        device_connection_t c; online(&c);
        const uint64_t old_epoch=c.supervisor.connection_epoch;
        device_control_envelope_t e={{conflicts[i],recoverable},true}; handle(&c,&e);
        assert(c.events==1 && !c.revoked);
        assert(c.supervisor.state==DEVICE_CONNECTION_STATE_BACKOFF);
        assert(c.supervisor.block_reason==DEVICE_CONNECTION_BLOCK_NONE);
        uint64_t epoch=0;
        assert(device_connection_supervisor_poll(&c.supervisor,c.supervisor.backoff_until_ms,0,&epoch)
               ==DEVICE_CONNECTION_ACTION_CONNECT_CACHED);
        assert(epoch>old_epoch);
        assert(device_connection_supervisor_transport_ready(&c.supervisor,epoch));
        assert(device_connection_supervisor_authenticated(&c.supervisor,epoch,2000));
        assert(c.supervisor.state==DEVICE_CONNECTION_STATE_ONLINE);
    }
    const char *blocked[]={"authentication_failed","invalid_pcm_sequence","unknown_error"};
    for(unsigned i=0;i<3;++i) {
        device_connection_t c; online(&c);
        device_control_envelope_t e={{blocked[i],false},true}; handle(&c,&e);
        assert(c.supervisor.state==DEVICE_CONNECTION_STATE_BLOCKED);
    }
    device_connection_t c; online(&c);
    device_control_envelope_t malformed={{"audio_input_busy",true},false}; handle(&c,&malformed);
    assert(c.supervisor.state==DEVICE_CONNECTION_STATE_BLOCKED && c.events==0);
    online(&c);
    device_control_envelope_t revoked={{"device_revoked",false},true}; handle(&c,&revoked);
    assert(c.revoked);
    online(&c);
    device_control_envelope_t asr={{"asr_provider_failed",true},true}; handle(&c,&asr);
    assert(c.supervisor.state==DEVICE_CONNECTION_STATE_ONLINE);
}
'''
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'recovery.c'
            source.write_text(driver)
            self.compile_run([source, EZ / 'device_connection/device_connection_supervisor.c'],
                             ['device_connection/include'])

    def test_conflicting_capture_is_cancelled_and_stale_error_is_ignored(self):
        handler = definition((EZ / 'ez_device.cc').read_text(), 'bool EzDevice::MediaEvent(')
        branch = definition(handler, 'if (event.type == DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR)')
        driver = r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGW(...) ((void)0)
enum { DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR };
struct Event {
    int type=DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR; uint64_t connection_epoch=2;
    struct { struct { const char *code; bool recoverable=false; } remote_error; } value;
};
struct Application {
    std::vector<std::function<void()>> tasks;
    static Application& GetInstance() { static Application a; return a; }
    void Schedule(std::function<void()> task) { tasks.push_back(task); }
    void Run() { auto pending=std::move(tasks); tasks.clear(); for(auto &f:pending) f(); }
};
struct Device {
    uint64_t connection_epoch_=2; bool conversation_active_=true,capture_failed_=false;
    std::mutex capture_mutex_; std::string detail_; unsigned cancelled=0,sealed=0;
    struct { void Clear() {} } dialogue_;
    void StopCapture() { if(capture_failed_) ++cancelled; else ++sealed; }
    void ProjectDialogue() {} void ProjectState() {}
    bool Handle(const Event& event) { BRANCH return false; }
};
int main() {
    for(const char *code:{"audio_input_busy","pcm_stream_mismatch"}) {
        Device d; Event e; e.value.remote_error.code=code;
        assert(d.Handle(e)); assert(d.cancelled==0); Application::GetInstance().Run();
        assert(d.cancelled==1 && d.sealed==0 && !d.conversation_active_);
        assert(d.detail_=="语音流冲突，正在重新连接 EZ");
    }
    Device stale; Event e; e.value.remote_error.code="audio_input_busy";
    assert(stale.Handle(e)); stale.connection_epoch_=3; Application::GetInstance().Run();
    assert(stale.cancelled==0 && stale.sealed==0 && stale.conversation_active_);
    Device empty; e.value.remote_error.code="asr_empty_transcript";
    assert(empty.Handle(e)); Application::GetInstance().Run();
    assert(empty.detail_=="没有听到内容，请说 Hi, ESP 重试");
}
'''.replace('BRANCH', branch)
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'abort.cc'
            source.write_text(driver)
            self.compile_run([source], [], cpp=True)
