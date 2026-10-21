# EZ 0.32 BOX-3 语音分段补丁

2026-10-21 用户决定：本机仅使用原版 EZ，不再修改或部署 Runtime。
此处补丁仅保留为上游 PR 的历史来源，60 字自动分段已提交 [上游草稿 PR #1](https://github.com/crazymad-apps/ez-assistant/pull/1)，等待上游评审；已移除本机补丁启动脚本。
当前原版每次 `speak` 最多 120 字符，不能将此补丁描述为当前已启用功能。

上游基线 `crazymad-apps/ez-assistant` 的 `a23d467698661a546d6002b6b4aefd272086b899`。
补丁位于 Runtime 私有 `speak` 工具，不改变设备协议、数据库结构、Provider 或绑定身份。

单次正文接受最多 1200 个 Unicode 字符，Runtime 按标点合并短句，每个 TTS 请求最多 60 字符。
整轮仍最多 20 个实际 TTS 片段，提交前先检查剩余配额；派生唯一子段 ID，按序等待 Host 接纳。
用户打断后停止余段；部分成功后的失败返回已排入字符数，提醒模型不得重播前缀。
不把整段回复裁为原 Bridge 的 240 字安全上限。每个 TTS 请求仍满足官方 Host 的 120 字硬限制。

在对应源码上应用 `git apply /absolute/path/speak-60.patch`，使用 Rust 1.98。
验证：`cargo test -p assistant-runtime --locked`、`cargo clippy -p assistant-runtime --all-targets -- -D warnings`。
构建 Host 前，按上游流程构建 `apps/desktop` 前端，再通过 `EZ_ASSISTANT_WEB_DIST` 编入同版本静态资源。
以上构建命令仅供源码评审，不作为本机部署或启动指引。
