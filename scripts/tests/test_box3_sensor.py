"""Run the production SENSOR driver with fake I2C/GPIO, never open hardware."""
from pathlib import Path
import tempfile
import unittest

import test_box3_display

ROOT = Path(__file__).resolve().parents[2]


class Box3SensorTests(unittest.TestCase):
    compile_run = test_box3_display.Box3DisplayTests.compile_run

    def test_official_measurement_radar_defaults_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            stubs = Path(directory)
            for name in ["driver/i2c_master.h", "driver/gpio.h", "esp_log.h", "esp_timer.h",
                         "freertos/FreeRTOS.h", "freertos/task.h"]:
                path = stubs / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "sensor_platform.h"\n')
            (stubs / "sensor_platform.h").write_text((ROOT / "scripts/tests/ez_fixtures/box3_sensor_platform.h").read_text())
            self.compile_run((ROOT / "scripts/tests/ez_fixtures/box3_sensor_test.cc").read_text(),
                             ["-I" + str(stubs), "-I" + str(ROOT / "main/boards/espressif/esp32-s3-box-3")])
