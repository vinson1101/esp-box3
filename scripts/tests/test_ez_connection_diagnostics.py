"""Production diagnostic branches with fake wire/TLS boundaries; never opens a device."""
from pathlib import Path
import runpy
import tempfile
import unittest

import test_ez_native as native
from test_startup_wake_word_hint import definition

ROOT = Path(__file__).resolve().parents[2]
LOG = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
static std::vector<std::string> logs;
template<typename... T> static void log(const char*,const char* fmt,T... values) {
    char b[512]; std::snprintf(b,sizeof(b),fmt,values...); logs.emplace_back(b);
}
#define ESP_LOGI(...) log(__VA_ARGS__)
#define ESP_LOGW(...) log(__VA_ARGS__)
static bool logged(const char* s) { for(auto& l:logs)if(l.find(s)!=std::string::npos)return true; return false; }
'''


class ConnectionDiagnosticsTests(unittest.TestCase):
    compile_run = native.EzNativeTests.compile_run

    def run_cpp(self, source):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'check.cc'
            path.write_text(source)
            self.compile_run([path], [], cpp=True)

    def test_heartbeat_logs_receipt_result_latency_without_nonce_or_payload(self):
        src = (ROOT/'main/ez/device_connection/device_connection.c').read_text()
        handler = definition(src, 'static void online_control_handle(')
        branch = definition(handler, 'if (strcmp(message_type, "ping") == 0)')
        driver = LOG + r'''
using wss_transport_result_t=int;
static constexpr int DEVICE_PROTOCOL_RESULT_OK=0,DEVICE_CONNECTION_FAILURE_PROTOCOL=1,
    WSS_TRANSPORT_RESULT_PROTOCOL_ERROR=9,WSS_TRANSPORT_RESULT_OK=0,
    DEVICE_CONNECTION_MESSAGE_ID_BYTES=24,DEVICE_CONNECTION_AUTH_MESSAGE_MAX_BYTES=2048,
    DEVICE_CONNECTION_MESSAGE_RANDOM_BYTES=18,DEVICE_CONNECTION_SEND_TIMEOUT_MS=5000;
struct device_application_heartbeat_t { char nonce[32]; };
struct device_control_envelope_t { bool valid=true; };
struct device_connection_t { struct { uint64_t connection_epoch=2; } supervisor; void* transport=nullptr; };
static uint64_t clock_ms=1000;
static uint64_t monotonic_ms(){return clock_ms;}
static unsigned sends=0,failures=0;
static int send_result=0;
static int device_application_heartbeat_decode_ping(const device_control_envelope_t* e,device_application_heartbeat_t* h){
    std::strcpy(h->nonce,"PRIVATE_NONCE");return e->valid?0:-1;
}
static bool create_random_token(unsigned,char* p,size_t){std::strcpy(p,"PRIVATE_ID");return true;}
static int device_application_heartbeat_encode_pong(const char*,const device_application_heartbeat_t*,char* p,size_t,size_t* n){
    std::strcpy(p,"PRIVATE_PAYLOAD");*n=std::strlen(p);return 0;
}
static int wss_transport_send_text(void*,const uint8_t*,size_t,unsigned){++sends;clock_ms+=7;return send_result;}
static int transport_failure(int r){return r;}
static void record_failure(device_connection_t*,int,int){++failures;}
static void handle(device_connection_t* connection,const device_control_envelope_t* envelope){
    const char* message_type="ping";
''' + branch + r'''
}
int main(){
    device_connection_t c;device_control_envelope_t e;
    handle(&c,&e);
    assert(sends==1 && failures==0 && logged("Heartbeat ping epoch=2"));
    assert(logged("Heartbeat pong epoch=2 result=0 elapsed_ms=7"));
    send_result=11;handle(&c,&e);
    assert(sends==2 && failures==1 && logged("Heartbeat pong epoch=2 result=11 elapsed_ms=7"));
    e.valid=false;handle(&c,&e);assert(sends==2 && failures==2);
    assert(!logged("PRIVATE_"));
}
'''
        self.run_cpp(driver)

    def test_tls_error_results_remain_distinct_and_include_errno_snapshot(self):
        src = (ROOT/'main/ez/wss_transport/wss_transport.c').read_text()
        methods = '\n'.join(definition(src,s) for s in ['static wss_transport_result_t tls_read_some(', 'static wss_transport_result_t tls_write_all('])
        driver = LOG + r'''
#include <cerrno>
using wss_transport_result_t=int;
static constexpr int WSS_TRANSPORT_RESULT_OK=0,WSS_TRANSPORT_RESULT_TIMEOUT=3,
    WSS_TRANSPORT_RESULT_CONNECTION_CLOSED=11,WSS_TRANSPORT_RESULT_CANCELLED=12,
    MBEDTLS_ERR_SSL_WANT_READ=-1,MBEDTLS_ERR_SSL_WANT_WRITE=-2,MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY=-3;
struct wss_transport_t {int tls=0;};
static int tls_result=0;
static bool cancelled=false;
static bool operation_cancelled(wss_transport_t*){return cancelled;}
static int mbedtls_ssl_read(int*,uint8_t*,size_t){errno=54;return tls_result;}
static int mbedtls_ssl_write(int*,const uint8_t*,size_t){errno=54;return tls_result;}
static int wait_for_socket(wss_transport_t*,bool,uint64_t){return WSS_TRANSPORT_RESULT_TIMEOUT;}
''' + methods + r'''
int main(){
    wss_transport_t t;uint8_t b[2]{};size_t n=0;
    tls_result=2;assert(tls_read_some(&t,b,2,&n,100)==0 && n==2);
    assert(tls_write_all(&t,b,2,100)==0);
    tls_result=0;assert(tls_read_some(&t,b,2,&n,100)==11);
    assert(logged("TLS peer closed result=0 errno_snapshot=54"));
    tls_result=MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;assert(tls_read_some(&t,b,2,&n,100)==11);
    tls_result=-77;assert(tls_read_some(&t,b,2,&n,100)==11);
    assert(logged("TLS read closed result=-77 errno_snapshot=54"));
    assert(tls_write_all(&t,b,2,100)==11 && logged("TLS write closed result=-77 errno_snapshot=54"));
    tls_result=MBEDTLS_ERR_SSL_WANT_READ;assert(tls_read_some(&t,b,2,&n,100)==3);
    cancelled=true;assert(tls_read_some(&t,b,2,&n,100)==12);
}
'''
        self.run_cpp(driver)

    def test_collector_keeps_close_tls_heartbeat_and_rejects_content(self):
        allowed = runpy.run_path(str(ROOT/'scripts/observe_box3.py'))['ALLOWED']
        for line in [
            'W (1) EZTransport: WebSocket close code=1001 reason=heartbeat timeout',
            'W (2) EZTransport: TLS peer closed result=-30848 errno_snapshot=54',
            'W (3) EZTransport: TLS read closed result=-77 errno_snapshot=54',
            'W (4) EZTransport: TLS write closed result=-77 errno_snapshot=54',
            'I (5) EZConnection: Heartbeat ping epoch=2',
            'I (6) EZConnection: Heartbeat pong epoch=2 result=0 elapsed_ms=1',
            'W (7) EZConnection: Failure epoch=2 category=0 transport=11 rx_idle_ms=3',
            'I (8) EZDevice: Boot toggle state=6',
        ]:
            self.assertIsNotNone(allowed.search(line), line)
        for line in ['transcript: PRIVATE', 'PCM: PRIVATE', 'Pairing code: PRIVATE',
                     'I (1) EZDevice: Pairing code: PRIVATE', 'credential: PRIVATE']:
            self.assertIsNone(allowed.search(line), line)
