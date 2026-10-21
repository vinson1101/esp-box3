# BOX-3 接入 EZ 助手

公开集成状态见 [EZ `.28` 版本说明](releases/esp-box3-ez-28.md) 和 [技术计划](ESP_BOX3_EZ_TECHNICAL_PLAN.md)。

## 链路

```text
BOX-3 麦克风 → AFE/VADNet → 20ms PCM 帧 → TLS/WebSocket → EZ 设备网关
                                                        ↓
                                                EZ ASR → 当前助理会话
                                                        ↓
BOX-3 屏幕/扬声器 ← 正文/PCM 播放与状态 ← EZ TTS/终端输出
```

- 板端保留 Hi, ESP 本地唤醒及长按 Boot 热点配网入口；换网后发现主机，不绑定当前测试网络。
- 配对、认证和媒体协议参考 `vinson1101/agent-robot` 固定来源；使用证书指纹、签名、消息去重及连接代际隔离，不在源码写设备身份。
- 单击 Boot 的收音/打断沿适配层现有状态机执行；旧播放不得污染新录音。ASR 接管确认与最终 Runtime 输入确认分别处理。
- EZ 决定助理会话和语音服务配置。旧 Bridge 的 Paraformer 传输复用于本机 ASR 协议适配，配置与凭据位于仓库外。TTS 由 EZ 独立配置。
- 当前主页、SENSOR、音量三屏分别验收。传感器离线工作，不依赖 Agent；音量入口在最后一屏。

当前版本仅更改终端源码和本机 ASR 适配源码；不修改安装的 EZ Runtime。60 字自动分段方案保留为单独源码补丁/PR，合并和安装需另行处理。

## 私有运行数据

真实密钥、网络密码、配对私钥、NVS、数据库、录音/对白、串口原始日志和本机部署路径均不提交 GitHub。使用独立私有配置文件，通过运行时接口配置；不要将本机配置复制到示例或默认值。
官方出处与许可证见 [参考资料库](references/esp-box3/README.md)，ASR 适配器用法见 [说明](../scripts/ez_speech/README.md)。
