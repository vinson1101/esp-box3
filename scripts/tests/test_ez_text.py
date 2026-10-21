"""Offline wire validation and bounded conversation projection; no device/voice I/O."""
import os
from pathlib import Path
import unittest
import test_ez_native as native

ROOT = Path(__file__).resolve().parents[2]
EZ = ROOT / "main/ez"
FIXTURES = Path(__file__).parent / "ez_fixtures"

class EzTextTests(unittest.TestCase):
    def test_text_wire_schema_utf8_limits_and_preference(self):
        cjson = Path(os.environ.get("EZ_CJSON_INCLUDE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
        if not (cjson / "cJSON.c").is_file():
            self.skipTest("set EZ_CJSON_INCLUDE_DIR to managed cJSON")
        native.EzNativeTests.compile_run(self, [
            EZ / "device_protocol/device_control_envelope.c",
            EZ / "device_protocol/device_media_control.c", cjson / "cJSON.c",
            FIXTURES / "ez_text_test.c"], ["device_protocol/include", cjson])

    def test_run_input_output_association_cancel_duplicates_and_utf8_bound(self):
        native.EzNativeTests.compile_run(self, [FIXTURES / "ez_dialogue_test.cc"],
            [".", "device_protocol/include"], cpp=True)
