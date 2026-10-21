"""Bounded passive BOX-3 diagnostics. Opening USB serial may reset the device."""
import argparse
import datetime
import json
from pathlib import Path
import os
import re
import signal
import time


ALLOWED = re.compile(
    r"(EZTransport:|EZConnection:|EZDiscovery:|"
    r"EZDevice: (Boot toggle|Wake detected|Capture |Follow-up listening|Input (event=|accepted )|"
    r"Audio state=|Connection state=|Playback (start|end|discard|rejected|output rejected|stalled|ingress)|"
    r"Reply drained|VAD speaking=|Media state=|Host error code=)|"
    r"StateMachine: State:|app_init: (App version|ELF file)|"
    r"AfeAudioEngine: (Model|Capture AFE|VAD reset)|EspBox3Board: GT911|Box3Display: Three pages|"
    r"rst:|Guru Meditation|CORRUPT HEAP|assert failed|ESP_ERROR_CHECK failed)"
)


def main():
    import serial

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--port", default="/dev/cu.usbmodem101")
    args = parser.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=True)
    now = lambda: datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat()
    status = dict(pid=os.getpid(), started_at=now(), duration_seconds=3600,
                  metadata_only=True, automatic_voice=False, running=False,
                  serial_reader_closed=True, events=0, bytes=0)
    save = lambda: (args.output / "monitor-status.json").write_text(json.dumps(status, indent=2)+"\n")

    def stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    port = serial.Serial(port=None, baudrate=115200, timeout=.2)
    port.port = args.port
    port.dtr = port.rts = False
    pending = b""
    start = time.monotonic()
    try:
        with (args.output / "device.jsonl").open("x") as output:
            port.open()
            status.update(running=True, serial_reader_closed=False)
            save()
            print("Passive BOX-3 diagnostics ready; no automatic voice.", flush=True)
            while time.monotonic()-start < 3600 and status["bytes"] < 8*1024*1024:
                pending += port.read(4096)
                if len(pending) > 65536:
                    pending = pending[-4096:]
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode(errors="replace")).strip()
                    if not ALLOWED.search(line):
                        continue
                    item = json.dumps(dict(at=now(), line=line), ensure_ascii=False)+"\n"
                    output.write(item)
                    output.flush()
                    status["events"] += 1
                    status["bytes"] += len(item.encode())
                    status["last_event_at"] = now()
                    if "Audio state=" in line:
                        status["latest_audio_state"] = line
                    save()
            status["reason"] = "bounded_monitor_complete"
    except KeyboardInterrupt:
        status["reason"] = "stopped"
    finally:
        port.close()
        status.update(running=False, serial_reader_closed=True, ended_at=now())
        save()


if __name__ == "__main__":
    main()
