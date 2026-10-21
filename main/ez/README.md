# BOX-3 的 EZ Device Gateway 接入

协议、身份、密码学、上行缓存和 PCM 播放队列复用 `agent-robot` 的固定提交，
文件及原始哈希见 `SOURCE.json`，第三方 Monocypher 许可保留在原目录。
未复制参考板的引脚、显示或音频驱动。

`ez_device.*` 负责 BOX-3 应用状态与协议的衔接。网络回调直接复制 PCM 到
有界播放队列；专用工作任务通过现有 AudioService 排队播放，不另开 I2S。
`pcm_framer.h` 将 AFE 输出分成 320 样本帧，最后一帧仅补零。
取消时清空队列并递增 AudioService generation，迟到旧流不得重新播放。
身份读取错误保留原 NVS，不自动擦除。

编译只在 `CONFIG_EZ_DEVICE_GATEWAY` 下启用。共享 Opus 接口和其他板型的
音频路径保持原行为。mDNS 组件固定 1.11.3，TLS 必须保留对端证书以校验 pin。

主机检查：

```sh
EZ_CJSON_INCLUDE_DIR=/path/to/managed_components/espressif__cjson/cJSON \
  python3 -m unittest scripts/tests/test_ez_native.py -v
```

`scripts/tests/ez_fixtures` 来自同一参考提交；WebSocket 测试只保留帧编解码
部分。目标 TLS、SPAKE2、Wi-Fi、AEC 和扬声器仍需实机验证。
