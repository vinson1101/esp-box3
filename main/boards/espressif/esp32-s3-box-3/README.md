# ESP32-S3-BOX-3

## 简介

<div align="center">
    <a href="https://github.com/espressif/esp-box"><b> ESP-BOX GitHub </b></a>
</div>

ESP-BOX-3 是乐鑫官方开发的 AIoT 开发套件，搭载 ESP32-S3-WROOM-1 模组，配备 2.4 英寸 320x240 ILI9341 显示屏，双麦克风阵列，支持离线语音唤醒与设备端回声消除（AEC）功能。

## 硬件特性

- **主控**: ESP32-S3-WROOM-1 (16MB Flash, 8MB PSRAM)
- **显示屏**: 2.4 英寸 IPS LCD (320x240, ILI9341)
- **音频**: ES8311 音频 Codec + ES7210 双麦 ADC
- **音频功能**: 支持设备端 AEC (回声消除)
- **按键**: Boot 按键 (单击/双击功能)
- **其他**: USB-C 供电与通信

## 本项目触摸三屏

默认消息/微信显示使用 `Box3Display`，保留现有主页，左右滑动依次切换主页、SENSOR 监测页和媒体音量页。三页使用完整 320×240 区域，无常驻底部菜单。媒体音量永远最后；当前只提供本机音量加减与滑条（0–100，松手保存），未接入音乐源。第二屏进入后约每秒采集温湿度与雷达活动状态，离开后停止采集和页面刷新。Emote 保持原显示路径，尚未接入此三屏。

触摸按[官方 BSP](https://github.com/espressif/esp-bsp/blob/f259a6abd5808019b1903a422092a4234ca95a51/bsp/esp-box-3/esp-box-3.c)识别 GT911/TT21100，复用 I²C1 SDA8/SCL18，INT3。实机验收见[三屏部署记录](../../../../docs/deployments/2026-10-21-box3-ui.md)。

无声 UI 测试：先用同版本 LVGL 构建主机静态库，再设置 `LVGL_SOURCE_DIR`、`LVGL_HOST_LIB`、`LVGL_HOST_CONFIG` 并运行 `python3 -m unittest discover -s scripts/tests -p test_box3_display.py -v`。未配置主机 LVGL 时，UI 测试明确跳过；Codec 音量恢复检查仍可直接运行。

`box3_ui_font.cc` 仅包含本页所需字符，从依赖的 Noto common 20px/4bpp 提取，避免精简字体缺字，遵循随附 SIL OFL。复现：`python3 scripts/build_box3_ui_font.py --fonts-root <managed_components/78__xiaozhi-fonts>`，再按项目格式化该文件；不修改原资源包或供应商目录。主机 UI 检查同时验证全部新增文案的字形。

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

### 基本配置
- `Xiaozhi Assistant` → `Board Type` → 选择 `ESP BOX 3`

### UI风格选择

ESP-BOX-3 支持多种不同的 UI 显示风格，通过 menuconfig 配置选择：

- `Xiaozhi Assistant` → `Select display style` → 选择显示风格

#### 可选风格

##### 表情动画风格 (Emote animation style) - 推荐
- **配置选项**: `USE_EMOTE_MESSAGE_STYLE`
- **特点**: 使用自定义的 `EmoteDisplay` 表情显示系统
- **功能**: 支持丰富的表情动画、眼睛动画、状态图标显示
- **适用**: 智能助手场景，提供更生动的人机交互体验
- **类**: `emote::EmoteDisplay`

**⚠️ 重要**: 选择此风格需要额外配置自定义资源文件：
1. `Xiaozhi Assistant` → `Flash Assets` → 选择 `Flash Custom Assets`
2. `Xiaozhi Assistant` → `Custom Assets File` → 填入资源文件地址：
   ```
   https://dl.espressif.com/AE/wn9_nihaoxiaozhi_tts-font_puhui_common_20_4-esp-box-3.bin
   ```

   这是保留表情动画和模型的历史资源包；新版固件会忽略其中没有 Noto 元数据的旧 Puhui
   字体，并使用内置 Noto basic 字体。待新的 Noto 资源包上传后可直接替换此 URL。

##### 默认消息风格 (Enable default message style)
- **配置选项**: `USE_DEFAULT_MESSAGE_STYLE` (默认)
- **特点**: 使用标准的消息显示界面
- **功能**: 传统的文本和图标显示界面
- **适用**: 标准的对话场景
- **类**: `SpiLcdDisplay`

##### 微信消息风格 (Enable WeChat Message Style)
- **配置选项**: `USE_WECHAT_MESSAGE_STYLE`
- **特点**: 仿微信聊天界面风格
- **功能**: 类似微信的消息气泡显示
- **适用**: 喜欢微信风格的用户
- **类**: `SpiLcdDisplay`

### 音频功能配置

#### 设备端回声消除 (AEC)
- `Xiaozhi Assistant` → `Enable Device-Side AEC` → 启用

ESP-BOX-3 硬件支持设备端 AEC 功能，可有效消除扬声器播放声音对麦克风的干扰，提升语音识别准确率。

**运行时切换**: 双击 Boot 按键可在运行时开启/关闭 AEC 功能。

> **说明**: 设备端 AEC 需要干净的扬声器输出参考路径和良好的麦克风与扬声器物理隔离才能正常工作。ESP-BOX-3 硬件已做优化设计。

### 唤醒词配置

ESP-BOX-3 支持多种唤醒词实现方式：

- `Xiaozhi Assistant` → `Wake Word Implementation Type` → 选择唤醒词类型

推荐选择：
- **Wakenet model with AFE** (`USE_AFE_WAKE_WORD`) - 支持 AEC 的唤醒词检测

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 ESP-BOX-3 连接至电脑，并运行：

```bash
idf.py flash
```

## 按键说明

本仓库 BOX-3 默认构建启用本地唤醒模式：启动时加载随固件烧录的模型，
不访问官方激活、OTA 或对话后端。当前构建使用 `--wake-word hiesp`，唤醒词为
`Hi, ESP`。命中后屏幕显示唤醒词并播放提示音，保持本地检测；这不是 ASR 或对话回复。
本地唤醒在未联网和热点配网时也可使用。

### Boot 按键功能

#### 长按三秒
- 进入热点配网，保留已有设置；手机连接设备热点后访问 `http://192.168.4.1`。
- 在配网页保存新网络后退出配网；无需重新烧录。已在配网模式时不重复启动热点。

#### 单击
- **配网状态**: 进入 WiFi 配置模式
- **空闲状态**: 开始对话
- **对话中**: 打断或停止当前对话

#### 双击 (需启用设备端 AEC)
- **空闲状态**: 切换 AEC 开启/关闭

## 常见问题

### 1. 为什么需要设备端 AEC？
设备端 AEC 可以在本地实时消除扬声器播放声音对麦克风的干扰，在播放音乐或 TTS 回复时仍能准确识别语音指令。

### 2. 表情动画风格无法显示？
请确保已经配置了正确的自定义资源文件地址，并且设备能够访问该 URL 下载资源。

### 3. 如何更换 Wi-Fi？
长按 Boot 三秒进入热点配网；此操作不会恢复出厂设置或清除所有配置。
# EZ Assistant 构建模式

`esp-box-3-ez` 使用局域网 EZ Device Gateway，保留 `Hi, ESP`、设备端 AEC、
原表情和长按 Boot 三秒热点配网。Boot 单击开始/结束录音；联网后等待 EZ
开启添加设备，屏幕显示六位配对码。配对身份独立保存，换 Wi-Fi 不会清除身份。

```sh
python3 scripts/build.py espressif/esp32-s3-box-3 --name esp-box-3-ez \
  --language zh-CN --wake-word hiesp \
  --build-options-json '{"aec_mode":"device","wifi_provisioning":"hotspot"}'
```

此模式不执行原云端激活或 OTA。EZ 需配置可用的 ASR/TTS；本机 Paraformer
接入说明见 [scripts/ez_speech](../../../../scripts/ez_speech/README.md)。
配网仍由现有 BOX-3 Wi-Fi 实现管理，密钥和网络密码不写入固件。
`esp-box-3` 继续保留本地唤醒模式，作为排查与回退入口。

## EZ 自动收音结束（`.19`）

首次唤醒未开口等待约 5 秒，播完后的本地续听窗口约 5 秒；检测到语音后，
按 16 kHz / 320 样本的 20 ms 帧维护 75 帧（1.5 秒）滑窗，尾窗内最多 3 帧判声、
且当前帧非语音时封口提交。机制参考本地原 Bridge，使用 BOX-3 AFE 已去抖的判声，
不复制 VB6824/C3 的固定能量阈值或额外 400 ms 门槛。保持短回复可提交。

结束事件只投递一次到主任务，按录音 ID 和连接代际核对，不等待每秒 Tick，
不在音频回调里修改 UI。Tick 仅保留未开口、60 秒录音上限与失败兜底。
持续被判为语音的背景声仍可能延长收音；真实噪声、距离与停顿效果由用户实测，
不能仅凭主机帧测试宣告声学已验收。配网、EZ 身份、ASR/TTS 服务和三屏音量不变。

## SENSOR 第二屏迁移（`.28` 已烧录，待实机验收）

第二屏提供温度、湿度、雷达活动状态、连接/读取异常及温湿度更新时间。
使用独立 I²C0 SDA41/SCL40（100 kHz），温湿度地址 0x38，雷达地址 0x28、输出 GPIO21；
音频/触摸仍使用 I²C1。低优先级独立任务执行有界读写和 100 ms 测量等待，不占用主事件循环、音频任务或 LVGL 回调。
只在第二屏可见时轮询，离开时至多完成一次正在进行的读取，结果不会刷新隐藏页面。
温湿度校验忙/校准状态与 7 字节 CRC，失败清空读数并保留最后成功更新时间；
雷达采用原厂默认配置，等待 2 秒自检后才显示活动信号，缺失/失败不显示为“无人”。
没有有效器件时显示未连接，初始化失败不会终止主机；可在再次进入页面时重试。

来源：
- [原厂 BSP 与引脚](https://github.com/espressif/esp-box/blob/aae1b7a6d193d968cf53e42f291a60c4ca1c622c/components/bsp/src/boards/esp32_bsp_sensor.c)
- [原厂 Sensor Monitor](https://github.com/espressif/esp-box/blob/aae1b7a6d193d968cf53e42f291a60c4ca1c622c/examples/factory_demo/main/gui/ui_sensor_monitor.c)
- [AHT20 读取与 CRC](https://github.com/espressif/esp-iot-solution/blob/4962410c80a51cd5822da1d8d2041eb4285265f1/components/sensors/humiture/aht20/aht20.c)
- [AT581X 默认初始化](https://github.com/espressif/esp-iot-solution/blob/4962410c80a51cd5822da1d8d2041eb4285265f1/components/sensors/radar/at581x/at581x.c)

`box3_sensor.h` 是上述 Apache-2.0 驱动的裁剪迁移，改用 IDF 6.1 原生 `i2c_master`，
保留协议、默认寄存器序列和版权；许可证全文在
[已归档的 Apache-2.0 许可证](../../../../docs/references/esp-box3/sources/esp-box/LICENSE)。
不引入新组件，不复制旧 I²C 驱动与电源管理。保留当前三屏导航与系统字体，
按用户确认的原厂详情页保留上方温湿度与雷达卡片、返回键、下方红外学习区域；
未连接时沿用现有纯文字说明与 Noto 20px 字体，不显示插画。图像仅转换为 LVGL 9 格式，不重画。温湿度沿用原厂整数显示，
红/灰雷达图标配合“活动/安静”文字表示输出状态，“活动感应”说明帮助首次使用。
红外区域用中文说明遥控器对准底座学习开关，并明确标记待启用；没有虚假的学习按钮。
红外学习/发射、雷达启停与系统休眠联动、SD 与电池管理仍在后续阶段。
素材复现：`python3 scripts/build_box3_sensor_assets.py`，再格式化生成的 `box3_sensor_assets.cc`。

实机验收：断电连接 SENSOR 后开机，进入第二屏确认实际读数与雷达活动/保持结束；
退出/重入和左右切页正常；断网下仍能使用；无底座或读取异常时，语音与末页音量不受影响。
具体芯片/硬件版本、校准精度、雷达灵敏度、热插拔与真实语音并发尚未验证，不承诺热插拔。

迁移验证：主机检查 160 项（3 项因缺少可选主机依赖跳过，其余通过）；最终 UI/传感器定向检查全部通过。
使用真实 LVGL 验证原厂图标、整数读数、缺失/失败、自检与活动状态、返回/左右滑动和末页音量。
`esp-box-3` 与 `esp-box-3-ez` 均已构建通过，应用分区分别约剩 23% / 20%；检查结果只证明本地候选，不代表已合并、部署或实机验收。

`.28` 已从合并源码 `977e023` 构建并仅烧录 EZ 应用，启动、原身份重连与心跳通过；资源和绑定保留。底座真实读数/活动与触摸效果待用户，未自动发声；详见[部署记录](../../../../docs/deployments/2026-10-21-box3-onset-sensor.md)。
