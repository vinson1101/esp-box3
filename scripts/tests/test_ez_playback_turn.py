"""Production terminal handlers and real PCM FIFO; no hardware or Provider calls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_startup_wake_word_hint import definition

ROOT = Path(__file__).resolve().parents[2]
EZ = ROOT / 'main/ez'


class EzPlaybackTurnTests(unittest.TestCase):
    def test_interrupt_and_multisegment_reply_boundaries(self):
        implementation = (EZ / 'ez_device.cc').read_text()
        signatures = [
            'void EzDevice::Wake()', 'void EzDevice::StartCapture()', 'void EzDevice::BeginCapture()',
            'void EzDevice::StartFollowup()', 'void EzDevice::BeginFollowup()',
            'void EzDevice::CancelPlayback()', 'void EzDevice::RememberDiscarded(',
            'void EzDevice::DiscardPlaybackLocked()', 'bool EzDevice::MediaEvent(',
            'void EzDevice::PlaybackTask()',
        ]
        handlers = '\n'.join(definition(implementation, s) for s in signatures)
        fixture = (Path(__file__).parent / 'ez_fixtures/ez_playback_turn_test.cc').read_text()
        fixture = fixture.replace('// PRODUCTION_HANDLERS', handlers)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'cJSON.h').write_text('typedef struct cJSON cJSON;\n')
            (path / 'esp_heap_caps.h').write_text(
                '#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n'
                '#define heap_caps_malloc(n, c) malloc(n)\n#define heap_caps_free(p) free(p)\n')
            (path / 'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n')
            includes = [path, ROOT / 'main', EZ, EZ / 'playback']
            includes += [p for p in EZ.glob('*/include')]
            flags = ['-I' + str(p) for p in includes]
            obj = path / 'fifo.o'
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                '-c', str(EZ / 'playback/audio_playback_queue.c'), '-o', str(obj)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            source = path / 'check.cc'
            source.write_text(fixture)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
                str(source), str(ROOT / 'main/audio/wake_words/wake_word_audio_cache.cc'),
                str(obj), '-o', str(path / 'check')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for case in ('interrupt', 'reply', 'queued', 'diagnostics', 'drain', 'cancel_start'):
                with self.subTest(case=case):
                    result = subprocess.run([str(path / 'check'), case], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
