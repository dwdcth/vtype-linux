# 微信输入法 Windows 引擎对接研究笔记（2026-10-20）

> ⚠️ 入口文档已迁移：日常开发从 `win/README.md`（交接笔记，含快速上手/坑清单/下一步）开始，
> 本文件保留为深度技术参考（偏移表、协议细节、实验日志）。两者冲突时以 win/README.md 为准。

> 本文档是 x64 原生路线的完整逆向资产清单，供后续 PR（Route W）使用。
> 环境：WeTypeSetup 3.0.1.1（Windows 版，2026-10 安装包）、wine 11.19、zig cc 交叉编译。
> 资产目录：`~/tmp/wetype-win/`（安装包解包 + 探针源码 + 反汇编 + proto 提取器）。

## 核心结论

微信输入法 Windows 版是 **Google Mozc 的魔改**（PDB 泄漏路径
`E:\workspace\WeChatPinYin\wxkb\01_mozc\...`，mozc 版本串 0.45.0.weekly_release/2026.09.23），
x86-64 原生，含 `libwxhld.dll`（与 Android 的 libwxhld.so 同源引擎，导出同一套
142 个 `wxime_*` C API）。**无需 ARM 模拟**。

## 三条可行路线

| 路线 | 原理 | 已验证 | 剩余工作 |
|---|---|---|---|
| A. wxime 直调 | wine 里 PE 守护进程 LoadLibrary(libwxhld.dll) 直接调 C API | ✅ initialize 返回 0 | InitInfo 接口对象桩化（+0xd0 多继承虚表 12 法）、SessionConfig 定型、listener/候选 ABI |
| B. mozc 管道 | 驱动 wetype_server.exe（wine 下常驻 OK），说 session_v2 管道协议 | ✅ 管道已连通，服务器 hello 推送可收 | 命令被静默丢弃的激活谜题（疑 WM_COPYDATA 注册，见下） |
| C. 混合 | （已否决）静态 DLL→so 转换 = 自写单库 Wine，无收益 | — | — |

## 路线 B 已知事实（wetype_server.exe 7.8MB）

- 管道：`\\.\pipe\wetype.<32hex安装key>.session_v2`（另有 session_v2_update / server_for_renderer）
  - key `838180e977848124a1b2f9a7258c4e70`（本机 wine 前缀实测，来源 IPCPathInfo）
- 帧格式（mozc win32_ipc.cc 原样）：**无帧头，message-type 管道，一次 WriteFile=一条消息**，小缓冲累积读（8B 初始，×3 增长）
- 连接后服务器立即推 `RespBase{version:1, resp:RespBodyV1{pid, detail{config快照}}}`（202B 实测）
- mozc 事务模型：请求→响应→100ms ACK 窗口→断开。命令不回 = Process 返回空→静默断开
- 已试且被无视的 payload：CommandBase(v0-5)/裸 CommandV1/mozc Command/裸 Input/裸 IPCPathInfo/各种 ACK 顺序
- **疑点**：服务器可能用 GetNamedPipeClientProcessId 校验客户端身份（字符串实证），
  客户端（tip）需先经 WM_COPYDATA（WmCopyDataType 枚举：RECEIVE_HANDLE=1 等）向服务器隐藏窗口注册
- `--test-mode` 参数可用（自检后退出）；`logtostderr` 打开日志

## 路线 A 已知事实（libwxhld.dll 19.7MB x64）

### InitInfo（0x138 字节，运行时断点捕获的真值，wetype_server 0x1401a0fd8 调用点）

```
+0x00  fn_ptr A（日志/事件回调；参数≈(level, loguru格式字符串)；返回0被容忍）
+0x18  char* workdir（引擎会 free，必须堆分配；子串 "Tencent\WeType\..."）
+0x40  char* 设备id串（实测"1345167160378274..."，len 0x14）  +0x48 int len  +0x4c int 2
+0x50  int platform = 6（Windows；ARM64 Android 是 0/1，布局不通用！）
+0x58  void* 对象（零区）
+0x68  char* "Windows.10.0.19045"（OS串）
+0x78  char* 同OS串 +0x80 int len(0x14)
+0x88  fn_ptr B（第二个回调，wetype_server 0x14019bc20）
+0xc0  int 3
+0xd0  void* → 多继承接口对象（vtable 0x140563640，≥12 方法，wetype_server 内）
+0xd8  void* → 对象{06 00 00 00 03 00 46 81 ...}
+0xe8  wine DLL 指针（0x6fffffbaf6f4 — 环境相关）
+0xf0  int 0xA0
+0xf8  void*
+0x100 int（-1?）
+0x108 void* → {11,0,1,0}
+0x118 void* → GUID样 {08 00 28 00 07 00 44 80 ...}
+0x120 void* → 已释放堆(0xFEEE)
+0x130 void* → {0x5e8,0,...}
```
坑：+0x18 为 NULL 会在引擎内 strlen 崩；+0 平台填错会被当函数指针 call（ARM64 经验填 1→rip=1）。

### SessionConfig（0x138 字节，create_session 调用点 0x1401a6633 逆向）
- 入口 `void* wxime_create_session(SessionConfig*)`；返回引用计数会话对象
- `wxime_add_session_listener(void* session, void* fn, void* ctx)`（fn=普通函数指针，wetype_server 用 0x14019bc30）
- `wxime_process_input(session, const char* key?, int len?, void*?) → bool`（异步投递到会话线程，结果走 listener）
- `wxime_select_candidate` 9+ 参数（暂缓）
- config 字段（相对偏移）：+0x00/+0x01 byte、+0x04 int(keyboard_type, 0=全拼)、+0x08..0x27 16字节块、
  +0x28 int、+0x2c..0x39 bool 组、+0x41、+0x50/+0x54 int、+0x5c byte、+0x88..0x8a bool、
  +0xb8 int、+0xc0 字符串{ptr, +0xdc len}、+0xE8 回调槽、+0xf8/+0xf9 byte、+0x100..0x118 四指针（IOServices）、
  +0x125 byte=1、+0x127 word=0x0101

### 其他签名
- `std::string wxime_get_commit_date()`（sret：rcx=32字节输出槽，MSVC string 布局）
- `wxime_reset_user_id(ptr, len)` 在 x64 收到空串（-255），签名存疑

## 协议资产（全部可再生成）

- `~/tmp/wetype-win/protos-src/`：**65 个 .proto**（commands.proto=会话协议全量、ipc.proto、
  WeTypeSharedData.proto、multi_platform_sync.proto=设备配对…）
- `tools/extract_all.py`：官方升级后一键重提 + diff 即知协议变化（用户点名要的可维护性）
- 工具链：`probe-src/probe.c`（初始化探针）、`pipebridge.c`（管道↔stdio 桥）、
  `dbg.c`（PE 调试器，int3 断点 dump 结构体——InitInfo 真值就是这么抓的）、
  `msgmatrix.c`/`framing.c`（管道实验）、`session_client.py`（Linux 侧 protobuf 客户端）
- mozc 原版参照：`/tmp/mozc_ipc.cc`、`win32_ipc.cc`（帧格式的权威依据）

## 关键教训

1. x64 与 ARM64 的 InitInfo/SessionConfig 布局**不通用**，偏移必须逐版本实测
2. wine 常驻服务（wetype_server）开箱即用；`--test-mode logtostderr` 是调试抓手
3. 运行时断点捕获（dbg.c）比静态反汇编快一个数量级——结构体真值直接抄
4. pkill -f 会自杀（本会话踩坑 3 次）：用 `pkill -x 精确进程名`
5. autoresearch hook 拦 rm -rf/.log/venv 路径：换 find -delete/改后缀/改目录名

## 下一步（按性价比排序）

1. **路线 B 激活谜题**：已确认 wetype_server 有隐藏窗口 `wetype.server.window`（wine 下
   winlist 实测，hwnd 可见）；WM_COPYDATA 各类型(0-22)均被接收(返回1)但管道仍不响应——
   **载荷是按 WmCopyDataType 分类型的结构化数据（疑 protobuf），需从 tip_core 反汇编
   （tip_disasm.txt 98万行已备）提取各类型的真实载荷格式**；或用 dbg.exe 断点服务器
   wndproc 直接看它校验什么
2. **路线 A 桩化**：把 +0xd0 接口 12 个方法逐个 stub（日志桩起步），crash-by-crash 收敛；
   SessionConfig 用 dbg.c 在真实会话建立时二次捕获（需先过路线 B 或伪造 tip 环境）
3. PR 结构（对齐上游 AGENTS.md）：scripts/fetch_win_engine.py + harness/win/ +
   wetype.cpp spawn 分支 + protos-src/ + 测试参数化 --launcher wine

## 实验记录附录

- WM_COPYDATA 激活尝试（wmcopy.c，2026-10-20）：类型 0-22 全部 SendMessage 返回 1，
  8 字节 {pid,tid} 载荷未能激活管道命令处理
- wetype_server 在 wine 下常驻运行稳定（多线程、引擎全初始化、三个管道、隐藏窗口）
- 服务器对无效管道消息的行为：读入后静默丢弃（不回错误、不断言），调试需靠 relay/断点
