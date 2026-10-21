# EZ 的 Paraformer 接入

EZ Assistant 0.32 的 DashScope ASR 接口发送 HTTP 整句 WAV 请求；旧 Bridge 的
`paraformer-realtime-v2` 使用官方 WebSocket。此适配器仅转换这两种协议，
监听 `127.0.0.1:18743`，不运行 Agent、设备配对或 TTS。

`paraformer.py` 的传输实现取自本人的 `xiaozhi-openclaw-bridge` 提交
`da7d7c47967ffe58b035238e5ed8b829d982970d`，保留 TLS 校验和有界帧解析。
只接受 16kHz、单声道、PCM16 WAV，最多 60 秒、两个并发请求。
WebSocket 分片消息暂不支持；若服务开始发送分片，应添加有界重组后再使用。

运行命令（Python 标准库，不需要 pip）：

```sh
python3 scripts/ez_speech/server.py --config /private/path/speech.json
```

私有 JSON 文件必须为 0600，包含以下三个键；不要放在仓库里：

- `XOB_BAILIAN_API_KEY`：原百炼凭据。
- `XOB_BAILIAN_PARA_ASR_MODEL`：`paraformer-realtime-v2`。
- `XOB_BAILIAN_ASR_WS_URL`：`https://dashscope.aliyuncs.com/api-ws/v1/inference`。

EZ 当前账号的 `[speech.asr]` 保持 `provider = "dashscope"`、原模型和凭据，
仅将 `endpoint` 指向 `http://127.0.0.1:18743`，然后重新加载配置。
TTS 继续使用原来的独立接入地址、CosyVoice 模型及音色，不经过本适配器。

适配器必须与 EZ 同机运行。服务停止时 ASR 无法识别；`/health` 只能证明
进程运行，不能证明百炼可用。日志不输出请求、密钥或识别文本。
接口要求 Bearer 凭据，不提供浏览器 CORS 权限。

```sh
python3 -m unittest scripts/tests/test_ez_speech.py -v
```
