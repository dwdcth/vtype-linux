# [RFC] Wine Windows x86_64 原生引擎路线（Route W）

> 草稿：供 review 后由维护者发到 zhl-HFUT/vtype-linux issues。数据与细节见 `win/README.md`。

## 动机

现有路线在 x86_64 主机上通过 qemu-user 跑 ARM64 引擎，存在固定翻译开销；
微信输入法官方 Windows 版引擎（`wetype_server.exe`，mozc 魔改 x86-64 原生）可在 wine 下
完整常驻，为 x86_64 用户提供一条**无 QEMU、全原生执行**的路线。

## 已完成验证（本机 CachyOS x86_64 + wine 11.19，引擎 3.0.1.1）

| 环节 | 结果 |
|---|---|
| 引擎 wine 常驻 | 稳定，`logtostderr` 全初始化（0.01s，词库 ver:15） |
| session_v2 管道协议 | 全通（hello/CREATE_SESSION/SEND_KEY/候选/emit） |
| 选词 | `commit=CandidateWord{id}` 姿势实证（`int_value` 会卡死服务器连接） |
| `wetype_win_host.exe` 宿主 | ~420 行 C，零第三方依赖，行协议↔管道翻译，E2E 全绿 |
| 插件接入 | `wetype.cpp` +33 行：`WETYPE_WIN_ENGINE_DIR` 显式 / win64 目录兜底 |
| spawn 机制验证 | 不预起引擎，宿主自起 → READY(≈20s) → nihao→CAND(你好) → S→OK → C→OK → 再打字，ALL PASS |
| 云栈 | win64 安装携带 wetype-cloud.py + cloud/（配对/剪贴板与引擎解耦，照常可用） |

## 关键技术发现

1. 管道命令静默丢弃根因 = 服务端 PID 强校验
   （`application_info.process_id == GetNamedPipeClientProcessId(hPipe)`，vma 0x140180274）；
   宿主 PE 直连管道天然满足。
2. 选词必须回传最近一次 candidateUpdate 中该候选的 **id blob（~100B，逐次变化）**。
3. wine 不支持 `\\.\pipe` 枚举，管道 key 随发行树稳定（与安装路径无关），内置默认 + env 覆盖。

## 交付物

- `win/host/wetype_win_host.c`：宿主守护进程（zig cc / mingw 均可构建）
- `win/scripts/`：build.sh / install_win_engine.sh / run_host_test.sh / run_spawn_test.sh
- `win/README.md`：完整交接文档（反汇编证据、实验记录、坑清单）
- `fcitx5-wetype/src/wetype.cpp`：wine 分支（不装 win64 目录时行为零变化）

## 待办（对齐后推进）

- 插件真机验证（维护者机器：cmake 重编 + 装机试打）
- `fetch_win_engine.py`：钉 3.0.1.1 + SHA256 下载解包（腾讯 CDN 直链已验证 35MB/s）
- 打包形态（AppImage 并存 or 独立安装器）与文档位

## 疑问

1. wine 依赖（系统包 or 自带 wine）如何随安装分发？
2. win64/win 路线的定位：x86_64 用户的可选路线，还是长期并列一等公民？
3. `win/` 目录并入主仓库是否可接受（全部为自有代码 + 文档，零腾讯文件）？
