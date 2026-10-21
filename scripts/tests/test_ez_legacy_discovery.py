"""One-shot DNS discovery uses real wire packets; no EZ or speech service required."""
import ctypes
from pathlib import Path
import random
import shutil
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICE = '_ez-assistant._tcp.local.'
ADDRESS = socket.inet_aton('192.0.2.5')


def name(value):
    return b''.join(bytes([len(label)]) + label.encode() for label in value.rstrip('.').split('.')) + b'\0'


def rr(owner, kind, data):
    return owner + struct.pack('!HHIH', kind, 1, 10, len(data)) + data


def packet(instances=('a' * 32,), *, compressed=True, omit=None, duplicate_key=False):
    question = name(SERVICE) + struct.pack('!HH', 12, 1)
    service = b'\xc0\x0c' if compressed else name(SERVICE)
    records = []
    for identity in instances:
        instance = bytes([32]) + identity.encode() + service
        host = name(identity + '.local.')
        records.append(rr(service, 12, instance))
        fields = {'protocol_major': '1', 'protocol_minor': '0', 'path': '/device',
                  'installation_id': identity, 'certificate_fingerprint': 'b' * 64,
                  'pairing_available': 'true'}
        if omit:
            del fields[omit]
        txt = [f'{key}={value}'.encode() for key, value in fields.items()]
        if duplicate_key:
            txt.append(b'protocol_major=1')
        # Additional records deliberately precede SRV: parsing must follow names, not order.
        records.extend([rr(instance, 16, b''.join(bytes([len(t)]) + t for t in txt)),
                        rr(host, 1, ADDRESS),
                        rr(instance, 33, struct.pack('!HHH', 0, 0, 43125) + host)])
    return struct.pack('!HHHHHH', 0, 0x8400, 1, 1, 0, len(records) - 1) + question + b''.join(records)


class Record(ctypes.Structure):
    _fields_ = [('address', ctypes.c_uint32), ('port', ctypes.c_uint16),
                ('major', ctypes.c_uint16), ('minor', ctypes.c_uint16),
                ('path', ctypes.c_char * 16), ('installation', ctypes.c_char * 33),
                ('fingerprint', ctypes.c_char * 65), ('pairing', ctypes.c_bool)]


@unittest.skipUnless(shutil.which('cc'), 'host compiler unavailable')
class LegacyDiscoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        output = Path(cls.directory.name) / 'discovery.so'
        source = ROOT / 'main/ez/host_discovery'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(source / 'include'),
                        '-I' + str(source / 'private_include'),
                        '-I' + str(ROOT / 'main/ez/device_protocol/include'),
                        str(source / 'legacy_discovery_parser.c'), str(source / 'host_selection.c'),
                        '-o', str(output)], check=True, capture_output=True)
        cls.library = ctypes.CDLL(str(output))
        cls.library.legacy_discovery_parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_uint32, ctypes.POINTER(Record), ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def parse(self, data, capacity=16, source=ADDRESS):
        records = (Record * capacity)()
        count = ctypes.c_size_t()
        buffer = ctypes.create_string_buffer(data)
        status = self.library.legacy_discovery_parse(buffer, len(data),
            int.from_bytes(source, byteorder=__import__('sys').byteorder), records, capacity,
            ctypes.byref(count))
        return status, list(records)[:count.value]

    def test_compressed_and_uncompressed_wire_records(self):
        for compressed in (True, False):
            status, records = self.parse(packet(compressed=compressed))
            self.assertEqual(status, 0)
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0].port, 43125)
            self.assertEqual(records[0].path, b'/device')
            self.assertEqual(records[0].installation, b'a' * 32)
            self.assertTrue(records[0].pairing)

    def test_all_hosts_are_retained_for_existing_pairing_selection(self):
        data = packet(('a' * 32, 'c' * 32))
        self.assertEqual(len(self.parse(data)[1]), 2)
        self.assertEqual(self.parse(data, capacity=1)[0], 4)

    def test_incomplete_duplicate_or_wrong_interface_is_not_a_candidate(self):
        for key in ('protocol_major', 'protocol_minor', 'path', 'installation_id',
                    'certificate_fingerprint', 'pairing_available'):
            self.assertEqual(self.parse(packet(omit=key))[1], [])
        self.assertEqual(self.parse(packet(duplicate_key=True))[1], [])
        self.assertEqual(self.parse(packet(), source=socket.inet_aton('192.0.2.6'))[1], [])

    def test_truncation_pointers_transaction_and_question_fail_closed(self):
        data = packet()
        for size in range(len(data)):
            self.assertNotEqual(self.parse(data[:size])[0], 0)
        for offset, replacement in [(0, b'\x12\x34'), (2, b'\x86\x00'), (12, b'\xc0\x0c'),
                                    (len(name(SERVICE)) + 12, b'\x00\x01')]:
            broken = data[:offset] + replacement + data[offset + len(replacement):]
            self.assertNotEqual(self.parse(broken)[0], 0)

    def test_bounded_mutated_datagrams(self):
        rng = random.Random(912)
        data = packet()
        for _ in range(1000):
            broken = bytearray(data)
            for _ in range(rng.randint(1, 8)):
                broken[rng.randrange(len(broken))] = rng.randrange(256)
            status, records = self.parse(bytes(broken))
            self.assertIn(status, (0, 2, 4))
            self.assertLessEqual(len(records), 16)

    def test_udp_socket_is_ephemeral_bounded_deduplicated_and_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'lwip').mkdir()
            (root / 'lwip/sockets.h').write_text(
                '#include <arpa/inet.h>\n#include <sys/socket.h>\n#include <sys/time.h>\n#include <unistd.h>\n')
            (root / 'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n')
            (root / 'esp_timer.h').write_text('static long long esp_timer_get_time(void);\n')
            fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
static long long clock_us;
static int closes, sends, receives, fail_bind, fail_send, no_reply;
static const uint8_t response[] = {@PACKET@};
static long long esp_timer_get_time(void) {return clock_us;}
static int mock_socket(int a,int b,int c) {(void)a;(void)b;(void)c;return 7;}
static int mock_bind(int fd,const struct sockaddr *a,socklen_t n) {
    (void)n;assert(fd==7 && ((const struct sockaddr_in*)a)->sin_port==0);return fail_bind?-1:0;
}
static int mock_setsockopt(int fd,int level,int key,const void *v,socklen_t n) {
    (void)fd;(void)level;(void)key;(void)v;(void)n;return 0;
}
static ssize_t mock_sendto(int fd,const void *p,size_t n,int flags,const struct sockaddr *a,socklen_t len) {
    (void)fd;(void)flags;(void)len;
    assert(n==42 && ((const uint8_t*)p)[0]==0 && ((const uint8_t*)p)[1]==0);
    assert(((const struct sockaddr_in*)a)->sin_port==htons(5353));++sends;return fail_send?-1:(ssize_t)n;
}
static ssize_t mock_recvfrom(int fd,void *p,size_t n,int flags,struct sockaddr *a,socklen_t *len) {
    (void)fd;(void)flags;(void)len;clock_us+=100000;
    if(no_reply || receives++>=2) {errno=EAGAIN;return -1;}
    assert(n>=sizeof(response));memcpy(p,response,sizeof(response));
    struct sockaddr_in *s=(struct sockaddr_in*)a;s->sin_family=AF_INET;s->sin_port=htons(5353);
    s->sin_addr.s_addr=inet_addr("192.0.2.5");return sizeof(response);
}
static int mock_close(int fd) {assert(fd==7);++closes;return 0;}
#define socket mock_socket
#define bind mock_bind
#define setsockopt mock_setsockopt
#define sendto mock_sendto
#define recvfrom mock_recvfrom
#define close mock_close
#include "@SOURCE@"
int main(void) {
    host_discovery_record_t records[16];size_t count=99;
    assert(legacy_discovery_query(1500,records,16,&count)==0 && count==1);
    assert(closes==1 && sends==3 && clock_us==1500000);
    clock_us=0;no_reply=1;
    assert(legacy_discovery_query(1500,records,16,&count)==0 && count==0 && closes==2);
    assert(clock_us==1500000);
    fail_bind=1;assert(legacy_discovery_query(1500,records,16,&count)==2 && closes==3);
    fail_bind=0;fail_send=1;
    assert(legacy_discovery_query(1500,records,16,&count)==2 && closes==4);
    assert(legacy_discovery_query(0,records,16,&count)==1 && closes==4);
}
'''
            source = ROOT / 'main/ez/host_discovery'
            driver = root / 'socket.c'
            driver.write_text(fixture.replace('@PACKET@', ','.join(map(str, packet())))
                .replace('@SOURCE@', str(source / 'legacy_discovery.c')))
            binary = root / 'socket-check'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root), '-I' + str(source / 'include'),
                '-I' + str(source / 'private_include'),
                '-I' + str(ROOT / 'main/ez/device_protocol/include'),
                str(driver), str(source / 'legacy_discovery_parser.c'),
                str(source / 'host_selection.c'), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
