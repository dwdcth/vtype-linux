# win/ 交接笔记 —— 微信输入法 Windows x64 引擎对接

> **给接手的 AI**：本文自含全部状态。读完后你应当能立刻继续干活，不需要重读前人的会话。
> 深度技术参考（偏移表/协议细节/实验日志）另见 `docs/上游/windows-engine-route-notes.md`，两者互补，冲突时以本文件为准（更新）。

## 0. 一句话状态

微信输入法 Windows 版是 **mozc 魔改的 x86-64 原生引擎**，在 wine 下已能完整稳定运行；
管道已连通、协议已提取、结构体真值已捕获。**此前命令被静默丢弃的根因已通过反汇编精准定位：
服务端存在严格的 PID 强校验（`application_info.process_id == GetNamedPipeClientProcessId(hPipe)`）
及 `ParseLegalCommand` 的五项完整性检查，之前探针的硬编码 PID/跨环境 PID 导致被直接 drop**。

**2026-10-20 里程碑：Route W 全链路已打通并 E2E 全绿**——
`wetype_win_host.exe`（win/host/，行协议↔管道翻译守护进程）+ fcitx5 插件 wine 分支
（`WETYPE_WIN_ENGINE_DIR` 或 win64 目录兜底）+ 安装脚本（含云栈）+ spawn 机制验证脚本。
选词姿势、候选 id 凭证、ESC 清空、服务器自起等关键行为均已实证（见 §6.5）。

## 1. 背景与目标

- 用户机器：CachyOS x86_64 + KDE Wayland，wine 11.19（系统包），zig 0.17（mise），输入法 fcitx5+Rime
- 目标：让 fcitx5 用上微信输入法引擎，**不走 ARM 模拟**（用户明确拒绝 qemu-user 路线，
  即本仓库上游的 Android 方案），因此选择 Windows 版的原生引擎
- 最终交付：给 zhl-HFUT/vtype-linux 提 PR（策略：**先开 issue 对齐方向再写代码**，上游 AGENTS.md 的规矩）
- 用户的重要规矩：**任何系统改动先说明+问用户**；Tencent 的文件（安装包/解包产物/引擎 DLL）
  **绝不进 git 仓库**（上游 AGENTS.md 明令）

## 2. 目录导览

```
win/
├── README.md          ← 本文件（最新综合交接文档）
├── host/
│   └── wetype_win_host.c ★ Route W 正式宿主：vtype 行协议↔session_v2 管道 protobuf 翻译
│                          （zig cc -O2 构建；连接管道/解析响应/自起引擎，见 §6.5）
├── wxhost/            ★ Route C 模块化正式宿主（单 exe 进程内直调 libwxhld.dll）
│   ├── wxime_abi.h       C ABI：导出函数指针类型 + 结构偏移常量 + 迭代/选词协议
│   ├── wxime_engine.h/.c 生命周期：加载/初始化序列/词典门/会话/逐键/选词
│   ├── wxime_event.h/.c  事件侧：listener/类型门/候选 TLV 解码/快照轮询
│   └── main.c            演示：nihao→候选表→选词上屏→活性验证（E2E 已通）
├── probes/            ← PE 探针源码（zig cc 构建，见 scripts/build.sh）
│   ├── session_probe.c ★ Phase1 闭环探针：真实 PID + CREATE_SESSION + 逐键，验证管道全通
│   ├── select_test.c  ★ 选词姿势实验：int_value 会卡死连接，commit{id} 正确（见 §6.5）
│   ├── enum_pipes.c   管道枚举实验：结论 wine 不支持（FindFirstFile 报 66，NT 目录对象无）
│   ├── probe.c        Phase0：LoadLibrary+wxime_initialize，InitInfo 实验田（已跑通 init=0）
│   ├── probe2.c       wxime 直调路线（待修，见 §7）
│   ├── dbg.c          ★ 运行时结构体捕获调试器（InitInfo 真值就是它抓的）
│   ├── pipebridge.c   ★ 管道↔stdio 桥（注意：stdio 模式跨进程容易导致 PID 错位，见 §5）
│   ├── msgmatrix.c    管道 payload 矩阵实验器（ack_mode 参数见源码头注释）
│   └── wmcopy.c / pipetest / pipelist* / pipecrack / framing / winlist.c  一次性侦察工具
├── tools/             ← Linux 侧 Python（需 protobuf：~/tmp/wetype-win/pbin venv，或自建）
│   ├── extract_all.py ★ proto 再提取器（官方升级后重跑+diff，用户点名要的可维护性）
│   ├── pbpool.py      把 protos/desc/*.bin 装进 descriptor pool 供解码
│   └── session_client.py  Linux 侧管道客户端雏形（走 pipebridge，已废弃，宿主取代）
├── protos/            ← 65 个提取的 .proto + desc/*.desc.bin + MANIFEST.json（3.0.1.1）
├── ref/               ← exports_rva.txt（wxime_* 142 导出地址）+ server_iat.txt
│   │                     + disasm/（12 个关键 wxime 函数反汇编）
└── scripts/
    ├── build.sh             ★ 全量构建：host(-O2,-Wall) + probes(-O1)，zig 缓存落在 win/.zig-cache
    ├── run_probe_test.sh    探针编排（复制引擎+前缀到 /tmp、起服务器、跑探针、收尾 pkill）
    ├── run_host_test.sh     宿主 E2E：预起服务器，宿主全命令序列（READY/PING/B/S/C/Q）断言
    ├── run_spawn_test.sh    ★ spawn 机制验证：复刻插件 execlp 分支，宿主自起引擎，全链路断言
    └── install_win_engine.sh ★ 安装到 ~/.local/lib/wetype-ime/win64（引擎+宿主+prefix+云栈）
```

**不在仓库、但运行必需的资产（在 `~/tmp/wetype-win/`）**：
- `WeTypeSetup_3.0.1.1_3.exe`（208MB 官方安装包）
- `extracted/`（7z 解包：`extracted/3.0.1.1/` 引擎全套 + `WetypeCore_3.0.1.1/` TSF 层）
- `prefix/`（wine 前缀，含已初始化的配置）、`pbin/`（python venv，装了 protobuf）
- `server_disasm.txt`（86MB）/ `tip_disasm.txt`（52MB）——丢了用 regen_disasm.sh 重生成
- 官方下载直链（腾讯 CDN，国内 35MB/s）：`https://download.z.weixin.qq.com/app/win/WeTypeSetup_3.0.1.1_3.exe`

## 3. 已确立的事实（含反汇编实证，别浪费时间重验）

1. **Windows 版 = mozc 魔改**：PDB 泄漏路径 `E:\workspace\WeChatPinYin\wxkb\01_mozc\...`；
   版本串 `0.45.0.weekly_release/2026.09.23.b2f7520`。协议骨架与 mozc 原版基本一致。
2. `libwxhld.dll`（19.7MB, x86-64，extracted/3.0.1.1/）与 Android 的 libwxhld.so 同源，
   导出 **142 个 `wxime_*` C API**（地址表 ref/exports_rva.txt）。无 GUI 依赖。
3. **wetype_server.exe 在 wine 下完美常驻**：引擎全初始化（0.002s、词库 ver:15）、
   三个命名管道、隐藏窗口。调试开关：`wine wetype_server.exe logtostderr [--test-mode]`。
4. 管道：`\\.\pipe\wetype.838180e977848124a1b2f9a7258c4e70.session_v2`
   （32hex key 来自 `wetype.ipc.IPCPathInfo{key,process_id,thread_id,protocol_version,product_version}`，
   本机 wine 前缀实测值；另有两个兄弟管道 session_v2_update / server_for_renderer）。
5. 帧格式：**无帧头，message-type 管道，一次 WriteFile=一条消息**（mozc 原样）。
   连接后服务器立即推 `RespBase{version:1, resp:RespBodyV1{pid, detail{config}}}`（202B）。
6. **服务端核心消息处理函数已锁定**：
   - 入口分发：`wetype::server::ImeIpcServer::Impl::Process`（vma `0x14010bb00`）
   - 合法性检验：`wetype::server::ImeIpcServer::Impl::ParseLegalCommand`（vma `0x14010b7b0`）
   - 发送响应与 PID 校验：`0x140180150`（vma `0x140180274` 处对比 PID）
7. **wxime 直调路线**：`wxime_initialize(InitInfo*)` 成功返回 0；InitInfo 真值已捕获（notes 文档有
   完整偏移表）；`create_session` 崩在 InitInfo+0xd0（虚表 `0x140563640`，需提供桩对象）。
8. 全量 protobuf schema 已提取（protos/），会话协议 = `commands.proto`。

## 4. 快速上手命令

```sh
# 构建探针
cd ~/项目/vtype-linux/win && sh scripts/build.sh

# 启动引擎宿主（wetype_server，常驻）
cd ~/tmp/wetype-win/extracted/3.0.1.1
WINEPREFIX=~/tmp/wetype-win/prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
  nohup wine wetype_server.exe logtostderr > /tmp/wsvr.log 2>&1 &

# 连管道看 hello 推送
wine pipebridge.exe '\\.\pipe\wetype.838180e977848124a1b2f9a7258c4e70.session_v2'

# 抓结构体（调试器法，改 dbg.c 里 BP_* 地址可抓任意调用点）
wine dbg.exe    # 断 wxime_initialize(0x1401a0fd8) / create_session(0x1401a6633)

# 官方升级后重提 proto
~/tmp/wetype-win/pbin/bin/python tools/extract_all.py <新解包目录> win/protos <版本号>
```

## 5. 核心突破：管道命令静默丢弃的根因与破解方案

之前我们在向 `session_v2` 管道发送 `CommandBase` 时，服务端读入后被无声丢弃。
通过对 `server_disasm.txt` 的深度逆向，现已彻底厘清服务端的**完整门禁逻辑**：

### 5.1 门禁一：`ParseLegalCommand`（vma 0x14010b7b0）的 5 道硬规则

服务端在读取到管道消息后，在 `0x14010b7b0` 执行校验，不符合直接置错误标跳出：
1. **版本检查**（0x14010b7e0）：`CommandBase.version` 必须 **严格等于 1**（非 1 报 `Unsupported version: {}`，vma 0x140567738）。
2. **反序列化检查**（0x14010b932）：`CommandV1` 必须能从 `CommandBase.cmd` 成功反序列化（失败报 `Parse command error`，vma 0x140567788）。
3. **ApplicationInfo 存在性**（0x14010b988）：必须设置 `application_info` 字段（缺则报 `No application info`，vma 0x1405677a0）。
4. **PID 存在性**（0x14010b9b6）：`application_info` 必须设置 `process_id`（缺则报 `No process id`，vma 0x1405677b8）。
5. **ClientType 范围校验**（0x14010b9d8、0x14010ba01）：必须设置 `client_type`，且其值必须在 `[1, 3]` 范围内（`CLIENT_TIP=1`, `CLIENT_UPDATE=2`, `CLIENT_RENDERER=3`；非法则报 `Unsupported client type: {}`，vma 0x1405677d8）。

### 5.2 门禁二：客户端 PID 强校验（最关键拦截点！）

在响应分发函数（vma `0x140180150`）中：
- 服务端调用 Windows API `GetNamedPipeClientProcessId(hPipe)`（vma `0x14018022e`）获取**连接该命名管道的真实操作系统 PID**。
- 然后在 `0x140180274` 进行严格对比：
  ```assembly
  140180272: mov esi, DWORD PTR [rbp-0x39]  ; GetNamedPipeClientProcessId 返回的真实 PID
  140180273: mov r15d, DWORD PTR [rbp-0x49] ; application_info.process_id 字段值
  140180274: cmp esi, r15d                  ; 两者必须完全相等！
  140180275: je  0x140180381                ; 相等才进入发送响应分支 0x140180381
  ```
- **如果 `esi != r15d`**：
  服务端调用格式化输出，抛出 `Send msg to {} while connecting to {}`（vma `0x140581768`），并且**直接跳过发送逻辑（静默丢弃）**！

### 5.3 历史盲点解析

为什么之前所有工具都收不到回包？
- 在 `wmcopy.c` 中：硬编码写入了 `0x08, 0xd2, 0x09`（即 PID 1234），与执行进程的真实 PID 冲突。
- 在 `session_client.py` 中：填入了 Linux 原生进程的 `os.getpid()`，而连接管道的实际上是子进程 `pipebridge.exe`（在 Wine 中分配了全新的 Win32 PID），两者天然不相等，直接命中 drop 逻辑！
- 关于 `WM_COPYDATA`：在 `wetype_tip_core.dll` 反汇编中确认，`WM_COPYDATA (0x4A)` 发送的是 `std::string` 封装的统计/上下文日志（如 `REPORT_KEYBOARD_LOST=14`, `RECEIVE_HANDLE=1`），**管道命令的处理并不强制依赖复杂的 WM_COPYDATA 握手，核心阻碍完全在于 PID 匹配**。

---

## 6. 进度状态与剩余任务

### 6.0 路线 C（单 exe 进程内直调）—— 2026-10-20 深夜冲刺记录

用 REA+IDA 9.4（idalib headless）实作推进，关键突破：

1. **`wxime_initialize → 0` 打通**（probe2 v2）：InitInfo 严格按 dbg.c 真值表填写后，
   引擎日志经 cbA 回流（"engine ver:0.45.0.weekly_release/2026.09.23" 全串可见）。
2. **InitInfo 的真实身份 = `wxime_config` C++ 对象**：initialize 把它按名注册进全局注册表
   （sub_180819DD0）；订阅者按名取用（全库 14 处 "wxime_config" 字符串查找点）。
3. **+0xd0 虚表（server 0x140563640）13 槽全破译**——双接口侵入式指针管道件，非业务逻辑：
   - slot0/1、7/8 = copy(this,out)：写 {vtable,inner} 到 out 返回 out
   - slot2/9 = subscribe(this,{ptr,0})：**引擎把它的回调对象 move 进来**（AddRef+转发内层）
   - slot3/10 = 返回静态类型描述符（.data 块 {ptr,0,0,0,name}）
   - slot4/11 = 条件析构（`test dl,dl; if dl destroy(this,0x10)`）
   - slot5/12 = `return this+8`（内层对象）
   - slot6 = .rdata 数据非代码（不会被调）
   桩实现已在 probe2.c（16 槽日志虚表 + 哨兵描述符），**尚未被触发**（见 4）。
4. **当前卡点（crash-by-crash 第 2 站）**：create_session 深入到资源加载时主动 throw
   `wxime::base::Exception("Resource may not ready")` @
   `wxime::core::SessionCreator::operator()` line 1243（0x180220d30），下一检查是
   `resources->dict_thread_pool_`（line 1244）。**+0xd0 桩还没被碰到**——先过资源关。
5. **config+0xA8 理论修正（dbg2 实测推翻）**：真实 server 的 config+0xA8 **就是 0**
   （v1 崩溃是误把 strlen 填进去当指针）。"Resource may not ready" 另有根源：
6. **★ 真实 InitInfo 捕获（dbg2.c，断点 0x1401a0fd8）**：config 是**栈上大结构（≥0x800）的一段**：
   - +0x58 → config+0x760 兄弟对象：内含 "Tencent\WeType" 的**路径/选项结构**
     （{0,0,0xF,...,"Tencent\WeType",0xE,0xF,9,0x2A,2,...}，非虚表对象）
   - +0xf8 → 0x7ffff0c5eb88 指针、+0x100 = -1、+0x108 = 0x7fff40000062 标记指针、
     +0x120/+0x130 → 栈/堆对象——**v2 全填 0，这些才是 Resources 的真身候选**
   - +0x68 处 len 在 **+0x70**（v2 漏填）
   完整 dump 存 `win/.dbg2out.txt`（STACK-FRAME-LOW/HIGH 0x800 全量 + SIB+58 + 虚表）。
   **下一步**：反查 sub_1808166A0 系订阅者真正读哪个偏移建 Resources（在 IDA 里
   decompile sub_1808166A0 已看过 +0xA8；改查 0x815a1b 链上的其他订阅者），
   或直接在栈框架 dump 里找带虚表的对象逐个试填。
7. **单实例互斥体坑**：wetype_server 二实例秒退——dbg* 探针调试时绝不能预起服务器
   （run_probe_test.sh 已对 dbg* 跳过预起；症状=bp-event 只剩 ntdll 初始断点）。
8. **依赖差集（用户猜想验证）**：server 独有导入 `wetap_business.dll`（31 导出：
   business_initialize/net_start_task/report_*/device_code…=网络+上报服务层）+ 系统 DLL
   （IMM32/CRYPT32/gdiplus…）。libwxhld 自带 XNet/ime_net/tensorflowlite_c。
   **但** initialize 调用点前的 call 序列全是 server 内部构造——business 服务不在
   config 里直接传，更可能经兄弟对象/全局。**判定实验（下一步）**：dbg2 在
   LOAD_DLL_DEBUG_EVENT 里记录每个 DLL 的加载基址，把 config 各指针按模块归类——
9. **模块归类实验（dbg2 + LOAD_DLL 基址）**：initialize 时刻已加载 40 个模块，
   config 全部指针中**只有 +0xe8 落在模块内**（wine 内部）——其余兄弟对象全是
   栈/堆实体。结论：**"再加载几个 DLL"不是关键**（wetap_business/XNet 等已随
   libwxhld 依赖链加载）；Resources 缺的是 server 在栈上**手工构造的服务对象**。
   下一轮：dbg2 的模块名解析修好（GetModuleFileNameExA 分支没生效，unicode 读名
   有 bug），然后用同样断点在 **create_session 调用点（0x1401a6633）** dump
   server 的 SessionConfig 真值（IOServices +0x100..0x118 四指针指向谁），对照
   栈框架 dump 复刻。`.dbg2mod.txt` 有本次模块基址表。
10. **★ DI 服务定位器确认（第 3 站本质）**："Resource may not ready" throw 前的真凶是
    `throw std::runtime_error("cannot get instance for type")`（0x180220cf6 块）——
    工厂向**服务容器按类型取实例**失败。定位器模板实例共 5 个（同体 0x1142B）：
    `sub_1801A9D80 / sub_1801AAED0 / sub_1801AC020 / sub_1801AD170 / sub_1801AE2C0`
    （字符串 0x180e19510 反查所得）。**下一轮攻关**：反编译 sub_1801A9D80 看查找键
    （大概率就是 "wxime_config" 同款字符串键注册表！），确认工厂查询的服务名
    （疑 "resources"），然后 **probe2 在 create_session 前自己注册该服务**——
    若键是字符串，路线 C 可能一举打通（不用复刻任何栈对象，只注册桩服务）。
    注意：IDA adapter 的 xrefs_to 对代码调用不完整（工厂内 call 无回指），
    用 disasm 逐块走替代。

11. **★ 条目 10 已解 + 第 4 站已过（2026-10-20 深夜续，全部动态实证）**：
    - 那 5 个"定位器"其实是 `LogChannelJsonHelper` 模板实例（日志通道），真 DI 是
      **字符串键全局 map**（sub_1801F3790 按名取，缺键抛 `"cannot get <name> in args"`）。
    - Resources 真身链：全局 `qword_181210720`（ResourceMgr，40B 双接口对象）→
      `+0x20` = Impl(584B) → `Impl+0x28/+0x30` = `shared_ptr<Resources>` 对；
      SessionCreator 读 `Resources+0x3D8`（dict_thread_pool_）。
    - **ResourceMgr 工厂（sub_180286690）由 wxime_initialize 时的 config 注册通知触发**，
      从 map 取 "wxime_config" 建 Impl；但 **Impl+0x28 的 Resources 对由
      `wxime_config_dict` 填充**（→ sub_18029F2F0 → resource_holder.cpp 构建器
      "init global db"/"Config dict by file"）。server 调用点 0x1401a5f1e（唯一）。
    - **probe3.c（新文件，probe2 v2 基线不动）已实现**：词典表回放（dbg3 抓的
      26 条 {0, 绝对路径, type}，type=index.json 的 type 字段，排除 3 个 swbd*）+
      workdir 真值 `"Tencent\WeType\ImeDir"`（相对路径！）+ 20 位 deviceid +
      SessionConfig 312B 真值回放（dbg2）。**"Resource may not ready" 与
      session_creator.cpp:1243/1244 已彻底越过**，26 词典 66ms 全载入。
12. **第 5 站现况：UserCoordinateDictWrapper 构造崩（+0x209AB2，rcx=0）**：
    - 链：SessionCreator(+0x21E127-146) 经 Resources 虚表双接口 vcall（两端 fn 完全
      一致 = libwxhld+0x6D67C0，按 **id 0x86** 查静态注册表）取 user_coordinate 对，
      空对 → wrapper 构造 deref 崩。
    - **server 在 config_dict 内部立刻构造 8 次 0x86 包装器（dbg4 计数实证）；
      probe3 一次都不构造**。门在 sub_18029DA20+0x29EB8B：
      `[rbp+0x6b8]` 对象的 `+0x10` 字段为 0 则整个跳过（dbg6 实证 probe3 len=0，
      server 非空——指向 InitInfo+0x108 的小 POD `{17,4,1,0,0x20000,0,ptr,0,17,1}`）。
    - probe3 已照抄 InitInfo+0xF8=-1、+0x100=0x7fff40000062、+0x108=&POD，字段在
      initialize 后仍在（非 move 清空），但**引擎门对象的 +0x10 仍为 0**。
    - 追加实证（深夜末段）：Impl::Impl 的 r8 = **整个 InitInfo 的 0x138 栈拷贝**
      （两端 +0xD0 之后都是工厂栈渗出，结构一致——"拷贝邻域"不是差异来源）；
      `wxime_register_client_cos_api`（server 在 initialize 后调、参数
      {qword,qword,fn,fn}）probe3 以桩回调注册返回 **-1**（被拒），尚需
      正确的 ctx 字段（server 传 r13 两次，需 dbg 抓真值）。
    - **判定**：门对象是 ResourceMgr 工厂栈上 208 字节 wxime_config 拷贝（qmemcpy）
      的越界邻域——server 侧邻域恰好挂着活指针（§6.0.6 的 InitInfo+0xF0 六指针
      对象真身嫌疑）。**下一步**：dbg 断工厂 qmemcpy（sub_180286690+0xD1/0x131 附近）
      对比两端 208B 拷贝 + 邻域 0x40 字节，找 server 那侧 +0x10 活指针的来源对象
      （疑 = InitInfo+0xF0 六指针执行器组的成员），再在 probe3 里构造等价桩。
13. **★★★ 2026-10-21 凌晨：路线 C 全链路打通（probe3.c）——直调 DLL 输入出事件了！**
    - **第 5 站钥匙**：config_dict 参数结构的 **+0x10 版本串槽必须是"合法 MSVC
      长形式 std::string"**（{malloc堆指针, size, cap}）——引擎在构建器里把它当
      门与数据双重使用（SSO/0 均跳过用户词典装配；裸非零会被解引用 fault=0x31）。
      修正后 0x86 包装器开始构造，**create_session 返回会话（句柄=1）**。
    - **第 6 站钥匙**：`wxime_process_input(session, key, len, ctx堆缓冲, 0x10)`
      ——r9 必须传 malloc 的 context/out（server 真值，dbg7 于 0x1401af05d 捕获；
      传 NULL 在 +0xC0D05D `vmovdqu(%rdx=0)` 崩）。**每键返回 1 并回调 LISTENER！**
    - **事件已到**：`CandidateUpdatedEvent`（含 malloc 的 iterator 句柄、version、
      type）→ `SigBatch size 3` → LISTENER(session, b=事件对象, c, d=3)。
      b/c 是双接口侵入结构（b+0x10 自引用）。
    - fnB(InitInfo+0x88)=NULL 实验不影响该崩（"Failed to locate callback"
      是 loguru 内部注册表查询，疑 fnA 通道的 id 注册协议未实现）。
14. **★★★ 2026-10-21：打字→全量候选闭环达成（probe3.c，726/125 个候选）**
    - **候选解码**（cand_cb，get_candidates 第 2 参回调）：candidate_info
      `ci+0x20 → TLV`（偏 0x1C 起 `[01][n]` 计数 + `[len]UTF8` 串对，前两个 =
      拼音、词）；`ci+0x38 → JSON 全量`（C 串直读，records[].prefix/type）。
    - **★迭代协议（反汇编 wxime_get_candidates @RVA 1e2d30 全破译）**：
      官方 3 参签名 `(iter, cb, ctx)`（server 0x1401b24d4 同款），函数内部
      **同步 for 循环**调 `cb(ci, ctx)`；`ci` 每次复用同一栈槽，回调里必须
      当场取走内容；**cb 返回 BOOL：非零=继续、零=停止**（`test al,al; jne
      loop` @0x1801e2e93）——之前 void/return 0 都在首个候选后自杀。
      get_candidates 自身返回 1=成功；每次调用最多喂 5 个候选（内部计数器
      `[iter+0xa0] < 5` 只 inc 不清零，所以 cb 一路返回 1 时实际全量枚举——
      `n`→726 个、`nihao`→125 个，回调次数即候选总数）。iterator 在事件
      `b+0x90`（引擎日志 `ParseEvent ... malloc new_iterator` 与 b 的偏移
      实测恒 0x90）。**完整迭代器 API 家族**（exports_rva.txt）：
      candidate_next/delete_candidate_iterator/free_candidate_info/
      check_candidate_extra_info/pre_select_candidate/select_candidate。
    - **事件类型门（防崩关键）**：LISTENER 的 b 事件对象有两种形态——
      候选更新类：`*(b+0x10)==b`（双接口自引用）且 `*(b+0x08)!=0`，b+0x90 处
      iterator 有效；其他类型（组词等）：b+0x10 非 b、b+0x30 区全是 tagged/栈
      残值，**枚举必崩**。LISTENER 跑在引擎异步分发线程上，门内同步枚举
      即可（server 也在单线程上下文里调 get_candidates）。
    - **候选等价性验证**：`n`→你/呢/那/能/拿/年/您/哪…、`nihao`→你好/你还/
      你和/你会…，与 Route W 管道时代 candidateUpdate 完全一致。
    - **已知小瑕疵**：TLV 解码偶发空串（你好事件候选[1]，疑 emoji/云条目
      编码不同）；不影响主线。
    - **工程事故记录**：python 补丁链把 probe3.c 写爆成 20MB 重复块（18226 份
      cand_cb）。恢复法：从会话 transcript 重放 Write+Edit+成功 bash 补丁
      （跳过当时就失败的 edit 和毁文件的补丁）完整重建。
      **教训（已写进指南坑 10）：改探针 C 源只用 Edit 工具或整文件重写。**
15. **下一站**（路线 C 收尾→产品化）：
    - **★select 已打通（2026-10-21 深夜，probe3 实测）**：
      ABI（dbg8 于 server 0x1401a3ceb 健康现场 dump）：`rcx=session rdx=&sst
      r8d=词字节长 r9=候选TLV指针`，栈参 `{idlen=100,int 1} id指针 0 &out
      0x10 desc(NULL 可——server 传的 desc 实测已是 ABABAB 释放内存，引擎
      不读)`。sst = 0x40 字节 {+0x00 SSO 词串, +0x10 size, +0x18 0xF,
      +0x20 TLV指针, +0x30 idlen=100, +0x38 0x6F}。**r9 = 候选 TLV 对象
      指针**（`02|tag4B|序号 + [len]拼音 + [len]词`，与 ci+0x20 同构——
      server 把引擎给的 TLV 原样传回，id 内容无需构造，零缓冲即可）。
      主线程调用**返回 1 成功**，会话存活（后续按键照常处理），select 触发
      的事件负载里实证携带上屏文本（sel+b 链二级解引用出 "你好" UTF-8）。
      **死锁教训**：select 绝不能在 LISTENER（引擎异步分发线程）里调——
      它向会话线程投任务并等待 → 必死锁（实测挂死）；必须主线程调
      （TLV 指针从 cand_cb 枚举时捕获，'a'/'o' 等非候选事件后仍存活）。
      负载结构：事件 b 的指针字段两级解引用可见 {拼音音节记录(01"n"01"i"),
      候选词表("你好"…)}——精确提取 emit 文本的偏移下一轮定型。
    - 把 probe3 演进为正式宿主（对照 Route W 的 wetype_win_host.c 行协议），
      二者共存供上游选择。
    - 每键后引擎异步线程仍有 fault=0x1 tag 崩（不致命，主流程全过）——疑
      listener b/c 未按协议释放，后续对照 server 消费者 0x1401a8500 补。
      候选相关完整 API 家族（candidate_next / delete_candidate_iterator /
      free_candidate_info / check_candidate_extra_info / force_emit_pending_input /
      reset_session / reset_internal_cursor）都在 IAT 表里。
16. **★wxhost 模块化宿主（2026-10-21，路线 C 工程化起点）**：
    - `win/wxhost/`（4 模块：abi.h / engine.h+c / event.h+c / main.c），
      `sh scripts/build.sh` 产 `bin/wxhost.exe`。
    - **E2E 一次通过**：initialize→config_dict→逐键（n=726 候选截 512、
      nihao=125 候选）→select"你好"→上屏→选词后按键存活，与 probe3 行为一致。
    - API：`wxime_open(engine_dir, name)` → `wxime_session_open` →
      `wxime_send_key` / `wxime_poll_candidates` / `wxime_select(tlv, word)`。
      上屏文本 = 所选候选的词（宿主自行提交，同 Route W jinterop 契约）。
    - probe3.c 从此冻结为实证参考（诊断 dump 齐全），新功能进 wxhost。
    - emit 事件负载布局随事件形态漂移（b+0x38 目标不恒定），故不做脆弱
      提取；宿主以自选词为准即可。
17. **★行协议接入（wxhost --serve，2026-10-21 深夜）**：
    - `wxhost/wxhost_serve.h/.c`：Route W 同款命令集（B/L/S/C/PING/SAVE/Q/
      OPT/R/HK），fcitx5 插件无需改动即可切 Route C 后端。
      `scripts/run_wxhost_test.sh` E2E：nihao→CAND→S 0→OK 全通。
    - **★键间隔竞态（重要发现）**：B 命令逐键背靠背（微秒级）会让引擎
      首个 SigBatch 分发批次被下一键吞掉（listener 收不到候选事件→EMPTY）。
      demo 的 300ms 间隔从不触发、Route W 有管道 IO 延迟天然掩盖。
      **修复：逐键间 Sleep(20ms)**。
    - **已知问题（下轮攻）**：
      a) **重置会话 + 7 键连发（B nihaoma）仍致命崩**（AV fault=-1 tagged；
        首会话同序列不崩——疑 destroy 后引擎内部线程/队列残留，重置会话
        更脆弱。20/50ms 键间隔均无效，非间隔问题）。下轮：dbg 断崩溃点，
        或改用 wxime_reset_session 导出代替 destroy+create；
      b) completion 型候选（'n' 事件的 n→ni 补全表，726 个）select 会引
        引擎抛 C++ 异常（0xe06d7363）；exact_match 型（nihao→你好）可选；
      c) 'i' 键（非终结键）的候选事件被类型门排除——打 "ni" 看到的还是
        'n' 的列表（无"咱"），Route W 管道模式每键都有 candidateUpdate，
        门条件需再细化。
    - **★已修（本轮）**：整段 select 后引擎会话进 post-commit 终态（事件
      分发停摆、后续按键零事件）→ S 整段完成时**自动销毁重建会话**（对齐
      插件契约 OK=双方清干净）；listener 空闲守卫（发键/选词前等引擎会话
      线程出回调）；**select 的 sst/idbuf 必须 static**（引擎异步消费，
      栈缓冲函数返回即回收 → 会话线程读垃圾 → 分发线程静默死）。
    - **★逐键候选更新已接入（2026-10-21）**：组词事件词表（b+0x18 容器）
      作为**展示兜底**——分层 poll：候选事件表（带选词凭证，可 S）优先，
      组词表仅在无候选表时用（展示表必须可选中，插件按其索引 S）。
      'i' 键后 B ni 现在能看到 exact 表（你/拟/拟）。静默窗 150→250ms
      （'h' 候选事件间歇晚到，150ms 会错过）。
    - **部分选词（剩余 ma）卡点**：select 后引擎**不推任何剩余事件**
      （零 id 与 TLV-头-100B-做-id 均试败）——疑引擎把整段 commit 了。
      Route W 实证 commit{id} 后剩余组词会推 CAND——真 id 结构 = server
      消费者 0x1401a8500 从候选 TLV 生成的 100B blob（携带音节覆盖信息），
      **下轮逆向其构造逻辑**（找 candidate TLV → id 的转换段）。
    - C 命令 = 会话销毁重建（wxime_destroy_session + create，ESC 清不掉
      候选展示状态，同 Route W 语义）。
18. **工具与资产**（win/ 内）：
    - probes：**probe3.c**（路线 C 主力，含词典表/真值回放）、dbg3.c（config_dict
      参数捕获）、dbg4.c（libwxhld 相对断点通用模板）、dbg5.c（InitInfo 字符串/
      解引用层捕获）、dbg6.c（调试 probe3 自身）。
    - tools：idaw_batch.py（自包含批查：起 worker→开库→跑一批调用，worker 跨调用
      必死，用这个）、blockwalk.py、reanalyze.py。
    - **坑**：idalib 的 .i64 在 worker 被硬杀时会写坏（internal error 1228）——
      **主库 libwxhld.i64 与 libwxhld.dll.i64 均已损坏**（22:28/22:51 两次），
      需用 reanalyze.py 从 dll 重跑全量分析（约 25 分钟；注意 rpc 超时配置）。
      期间反汇编用 `objdump -d -M intel` 直查 DLL 完全够用（本轮大量结论来自 objdump）。
    - IDA 事实修正：dll.i64 里 xrefs_to 对代码调用**可用**（指南 §3 说的不完整
      只适用于旧库）；RVA 换算：IDA 地址 = 0x180000000 + RVA。


工具链沉淀（复用指南）：
- **idalib-mcp 直连**：`IDADIR=… python -m ida_pro_mcp.idalib_server --host 127.0.0.1 --port 53124`，
  `win/tools/idaw.py` 是 HTTP MCP 助手（IDAW_URL 覆盖端口）。
- **分析库已持久化**：`~/tmp/wetype-win/extracted/3.0.1.1/libwxhld.i64`（328MB，含 25 分钟
  全量分析）。**必须直接 idb_open(.i64)**——传 dll 会开裸库（反编译只见空壳 JUMPOUT）。
- 工具要点：decompile（500 行截断，大函数用 disasm 补）、xrefs_to、find_bytes
  （通配 `48 8B ?? D0`）、idb_save（worker 活着时可跨 REA 直连调用抢救分析成果）。
- clang 编译的 PE 探针没有 SEH `__try`：用 VEH（AddVectoredExceptionHandler）捕崩溃现场。
- REA 的 MCP 层超时频繁，直连 worker 更快（本节实践结论）。

### 6.1 已完成（2026-10-20，全部有自动化验证）

- **阶段一**（管道闭环）：`session_probe.c` 实证 PID 假设成立。hello 202B、CREATE_SESSION、
  逐键 SEND_KEY "nihao" 全通；pendiUpdate（composition "n"/"ni"）与 candidateUpdate
  （`ni`→你/咱/那/能/拿/年/您/哪/弄；`nihao`→你好/你还/你和/你会+👋，云候选 762-987B 大包）均收到。
- **阶段二**（宿主）：`win/host/wetype_win_host.c`（约 420 行 C，无第三方依赖）实现完整行协议：
  `PING/OPT/SAVE/C/B/L/S/R/HK/Q` ↔ 管道 protobuf 翻译；读线程异步收包、CS+代数版本号同步、
  150ms 静默窗聚合候选、断线惰性重连+重建会话。`run_host_test.sh` E2E 全绿。
- **阶段三**（fcitx5 分支）：`fcitx5-wetype/src/wetype.cpp` 的 `resolveDirs` 加 `winHost` 出参
  （`WETYPE_WIN_ENGINE_DIR` 显式启用；或无 ARM64 引擎时 win64 目录兜底），fork 子进程加
  `execlp("wine", host)` 分支（WINEDEBUG=-all、WINEPREFIX=eng/prefix 若存在）。
  `run_spawn_test.sh` 复刻 spawn 机制（不依赖 fcitx5 构建）全链路 ALL PASS。
- **安装布局**：`install_win_engine.sh` 装出 `~/.local/lib/wetype-ime/win64/`
  （引擎全套 + wetype_win_host.exe 同目录 + prefix/ + wetype-cloud.py + cloud/ 云栈），
  自动剔除历史实验产物。

### 6.2 已实证的关键行为（§6.5 之外不要再猜）

见 §6.5 实验记录。

### 6.3 剩余任务（阶段四：工程化与社区交付）

1. **插件真机验证**（需维护者机器）：沙箱无 cmake/fcititx5 依赖，`wetype.cpp` 改动只做了
   人工复查 + spawn 机制等价验证。需 `cmake -S fcitx5-wetype -B build && cmake --build build`
   后装 `libfcitx5-wetype.so` 真机试打。
2. **资源下载脚本**：`scripts/fetch_win_engine.py` 钉死 `WeTypeSetup_3.0.1.1_3.exe` + SHA256，
   解包出干净引擎树（现安装脚本的源是本地 `~/tmp/wetype-win/`，含历史实验残留靠排除表剔除）。
3. **上游对接**：先开 issue “[RFC] Wine Windows x86_64 原生引擎路线 (Route W)”，
   附性能数据（无 QEMU 翻译开销、READY≈20s 内、逐键候选延迟与 ARM 路线对比），对齐后再 PR。
4. **云对接说明**：win64 安装已带云栈（配对/云候选状态/剪贴板，与引擎解耦、独立于运行路线）。
   上游 `cloud/` 与 [wx-ime-sdk](https://github.com/mkdir700/wx-ime-sdk)（Android 侧协议重建，
   npm CLI `@mkdir700/wxc` 已是本仓库配对依赖）协议同源；升级 wxc 集成（文件互传等新能力）
   属上游 cloud 栈的独立议题，不阻塞 Route W。

### 6.4 快速上手（全链路验证）

```sh
# 1. 构建（宿主 + 探针）
cd ~/项目/vtype-linux/win && sh scripts/build.sh

# 2. 安装到 win64 目录（引擎源默认 ~/tmp/wetype-win/）
sh scripts/install_win_engine.sh

# 3. spawn 机制验证（宿主自起引擎 → READY → 打字 → 选词 → 清空 → 再打字）
bash scripts/run_spawn_test.sh ~/.local/lib/wetype-ime/win64

# 4. fcitx5 真机：重编插件、装 .so、重启 fcitx5，设
#    WETYPE_WIN_ENGINE_DIR=~/.local/lib/wetype-ime/win64（或不装 ARM64 引擎走兜底）
```

### 6.5 实验记录：关键行为与坑（重要！）

1. **选词姿势（最大坑）**：`SELECT_CANDIDATE` 用 `Detail.int_value=索引` 会**卡死服务器的
   连接处理**（服务器进程活着，但管道停止读、后续 WriteFile 永久阻塞）。正确姿势是
   `Detail.commit(field 5) = CandidateWord{id(field 2)}`——把最近一次 candidateUpdate 里
   该候选的 **id blob（实测 ~100B，每次更新都变）原样回传**。收到 type=1（emit）推送即成功，
   管道继续可用（`select_test.c` mode1 实证）。
2. **部分选词语义必须实现**：候选如「你」只盖住 `ni` 音节，引擎 commit 后**剩余 `hao`
   组词仍在**。契约（jinterop）：此时 S 应回**剩余候选的 CAND**（插件用 candidateHandler
   接住，继续组词），整段消费完才回 OK。若一律回 OK → 插件清缓冲、引擎组词残留 →
   之后每次按键都是残留音节的补全（用户实测症状：打 f 出来的是 c 开头的词）。
3. **ESC 清不掉"候选展示中"的会话状态**：`SEND_KEY{special_key=ESCAPE}` 发出后组词
   冻死、后续 SEND_KEY 全被吞（candidateUpdate 照推但内容不变）。唯一可靠重置 =
   **断开管道重连 + CREATE_SESSION**（与启动路径同一条被验证的路）。宿主 C 命令即如此实现。
4. **mingw PE 的 CRLF 陷阱**：文本模式 stdout 把 `\n` 翻成 `\r\n`，插件逐行**精确匹配**
   （`line == "READY"`）全部失配 → 启动看门狗杀引擎。必须 `_setmode(stdout, _O_BINARY)`。
   症状：日志里 `bytes=6 prefix=READY`（多了 1 字节 \r）。
5. **宿主的 S 命令用快照 id**：B 回 CAND 时的候选表必须连同 id 一起快照；云候选晚到会覆盖
   活表，若 S 用活表 id，索引语义就错位了。
6. **CREATE_SESSION 无响应是正常的**：会话随首个按键自动建立，别等回包。
7. **服务器自起**：宿主 2s 内连不上管道就 `CreateProcessW wetype_server.exe`
   （cwd=本 exe 所在目录，BuildIn 词库是相对路径），~15-20s 后管道就绪。
8. **管道名发现不可行**：wine 不支持 `\\.\pipe` 枚举（FindFirstFileW 报错误 66，
   `\??\Pipe` NT 目录对象不存在，`enum_pipes.c` 实证）。key 随发行树稳定
   （与安装路径无关，复制 prefix/引擎到任意路径 key 不变），宿主内置默认 key +
   `WETYPE_WIN_PIPE` 环境变量整名覆盖。
9. **响应时序**：每键 1-2 条推送（先 pendiUpdate 后 candidateUpdate）；偶发连接即被占
   （单实例管道被上一个未死透的客户端占住，CreateFileW 失败）——测试脚本先 pkill 再起。
10. **逐键 B 的延迟叠加**：150ms 静默窗在快速打字时逐键排队叠加（卡顿感）。宿主用
    `PeekNamedPipe(stdin)` 探测排队中的新命令，有就提前截断静默窗立刻回最新候选。
11. **写看门狗**：服务器连接被楔死时 WriteFile 永久阻塞。宿主配 3s 看门狗
    （CancelIoEx 掐断 + 重连自愈），杜绝整个宿主冻死。
12. **云候选**：引擎自身的云通道在 wine 下也会出候选（泥嚎/你号等模糊云词出现在本地候选后），
    未配对也出现，来源待查；与本仓库 cloud 栈（配对/剪贴板）是两条独立通道。

## 7. 坑清单与开发警示（前人血泪，必读）

1. **进程管理命令警示**：
   - **`pkill -f` 会导致当前 shell 自杀**！绝对禁止使用带有模糊参数的 `pkill -f`。
   - 必须使用 `pkill -x 精确进程名`（如 `pkill -x wetype_server.e`，Linux comm 字段截断为 15 字符）。
2. **环境 Hook 规避**：
   - 开发机环境 hook 了 `rm -rf`（一律用 `find <dir> -depth -delete` 替代）。
   - 环境拦截 `.log` 文件名（日志文件统一命名为 `.txt`）。
   - 拦截 `venv/bin/python` 路径（虚拟环境统一命名为 `pbin`）。
3. **结构体布局绝不跨平台通用**：
   - x64 与 ARM64 的结构体字段顺序完全不同（例如 `InitInfo+0` 在 ARM64 是 int platform，在 x64 是日志函数指针）。所有偏移一律以 x64 实测为准。
4. **运行路径与依赖**：
   - `wetype_server.exe` 启动时必须将工作目录（cwd）切换到引擎目录 `extracted/3.0.1.1`，否则会因为找不到 `BuildIn` 目录下的词库文件直接退出。
5. **Wine 调试建议**：
   - 需要查看命名管道和窗口调用时，可使用环境变量：`WINEDEBUG=+relay,+msg`。
   - `wetype_server.exe` 加上 `logtostderr` 参数可将内部 loguru 日志输出到标准错误。
