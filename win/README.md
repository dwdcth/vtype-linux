# win/ —— Windows 引擎路线（wine）

不用 QEMU 跑 ARM，直接把**微信输入法 Windows 版的官方引擎**跑在 wine 里。
引擎来自官方安装包（`wetype_installer.exe`，3.0.1.1），安装脚本负责解包，
仓库内不含任何腾讯二进制。

## 架构

```
fcitx5 插件 (libfcitx5-wetype.so)
  └─ wine wetype_win_host.exe        ← 本仓库代码（win/host/）
       └─ 命名管道 + protobuf        ← 协议逆向自官方（win/protos/）
            └─ wetype_server.exe     ← 腾讯官方组件，原样运行
                 └─ libwxhld.dll     ← 引擎本体（候选/组词/学习全在官方侧）
```

候选、组词、选词学习全部由官方 server 与引擎完成，宿主只做协议翻译
（插件行协议 ⇄ 管道 protobuf），因此候选质量与 Windows 原版一致。

## 安装

```sh
# 依赖：wine、交叉编译器（zig 或 mingw-w64-gcc）
sh win/scripts/build.sh                    # 产 win/bin/wetype_win_host.exe
sh win/scripts/install_win_engine.sh       # 解包官方引擎并装到 ~/.local/lib/wetype-ime/win64
```

fcitx5 插件发现 `~/.local/lib/wetype-ime/win64` 后自动走本路线
（设 `WETYPE_WIN_ENGINE_DIR` 可显式指定）。云功能（关联、剪贴板）与
ARM 路线共用 `wetype-cloud.py`，不受影响。

## 关键设计（wetype_win_host.c）

- **行协议**：与 ARM 路线宿主同款命令集（`B` 打字 / `S` 选词 / `C` 清空 /
  `PING` 等），fcitx5 插件零改动即可切换后端。
- **候选等待**：server 通过管道异步推 `candidateUpdate`，宿主按代数取最新
  表；`QUIET_MS`(100ms) 静默窗合并连发推送，插件有新按键排队时立即返回
  （快速打字不叠加延迟）。
- **无更新早退**：部分键（如补全形态）server 不推新表，等满
  `NOEV_MS`(600ms) 后沿用上次快照回发——候选窗不闪没，也避免等满
  2.5s 总超时（实测每键响应 ~110ms）。
- **选词凭证**：候选携带 server 分配的 id blob，`S` 必须回传「下发那份
  表」里的 id；快照同时存词与 id，`C`（会话重建）后旧快照作废。
- **C = 会话重建**：ESC 清不掉组词展示状态，唯一可靠重置是断开重连
  管道并重新 `CREATE_SESSION`。
- **崩溃自愈**：管道断开时宿主自动重连重建会话，插件侧另有引擎重启
  逻辑，双层兜底。

## 协议逆向资料

`win/protos/` 是从官方二进制还原的完整 protobuf 定义
（`desc/` 为编译后的 descriptor），`build_cmd`/`pipe_send` 按
`BaseMsg` 帧格式打包。字段语义的实证过程记录在提交历史里。

## 与 ARM 路线的关系

两条路线并存，由插件按安装情况自动选择：

| | ARM（harness/ + QEMU） | Windows（win/ + wine） |
|---|---|---|
| 引擎 | Android 3.5.4 `libwxhld.so` | Windows 3.0.1.1 `libwxhld.dll` |
| 运行 | QEMU 用户态模拟 ARM64 | wine 原生 x86_64 |
| 性能 | 模拟开销 | 原生速度，无 QEMU |
| 覆盖 | 主线，全部功能 | x86_64 + wine 环境 |

曾试过不经 server、进程内直调 `libwxhld.dll` 的单宿主方案，因引擎
候选事件的消费契约只对官方 server 完整开放（长句候选不完整、稳定性
差），已放弃；相关代码未入库。
