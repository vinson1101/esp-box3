import base64
import http.client
import io
import json
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ez_speech"))
import server


def audio_payload(rate=16000):
    data = io.BytesIO()
    with wave.open(data, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(bytes(640))
    return {"model": "paraformer-realtime-v2", "input": {"messages": [{"content": [
        {"input_audio": {"data": "data:audio/wav;base64," + base64.b64encode(data.getvalue()).decode()}}
    ]}]}}


class EzSpeechTests(unittest.TestCase):
    def test_pcm_contract_and_reject_invalid_audio(self):
        self.assertEqual(server.decode_audio(audio_payload(), "paraformer-realtime-v2"), bytes(640))
        for payload in (audio_payload(8000), {"model": "other"}, {"model": "paraformer-realtime-v2"}):
            with self.assertRaises((ValueError, KeyError)):
                server.decode_audio(payload, "paraformer-realtime-v2")

    def test_authenticated_http_contract_and_provider_failure(self):
        config = {"XOB_BAILIAN_API_KEY": "test-only", "XOB_BAILIAN_PARA_ASR_MODEL": "paraformer-realtime-v2",
                  "XOB_BAILIAN_ASR_WS_URL": "https://dashscope.aliyuncs.com/api-ws/v1/inference"}
        host = server.SpeechServer(("127.0.0.1", 0), config)
        worker = threading.Thread(target=host.serve_forever, daemon=True)
        worker.start()

        def request(token, body):
            connection = http.client.HTTPConnection(*host.server_address, timeout=3)
            connection.request("POST", server.ASR_PATH, json.dumps(body),
                               {"Authorization": "Bearer " + token})
            response = connection.getresponse()
            result = response.status, json.loads(response.read())
            connection.close()
            return result

        try:
            with patch.object(server, "_run_paraformer_realtime", return_value=("你好。", 1)) as provider:
                self.assertEqual(request("wrong", audio_payload())[0], 401)
                provider.assert_not_called()
                self.assertEqual(request("test-only", audio_payload()), (200, {"output": {"text": "你好。"}}))
                self.assertEqual(provider.call_args.kwargs["audio"], bytes(640))
                self.assertEqual(request("test-only", audio_payload(8000))[0], 400)
            with patch.object(server, "_run_paraformer_realtime", side_effect=OSError("private provider detail")):
                self.assertEqual(request("test-only", audio_payload()), (502, {"code": "AsrProviderFailed"}))
        finally:
            host.shutdown()
            host.server_close()
            worker.join()


if __name__ == "__main__":
    unittest.main()
