# IDA Pro 使用指南（win/ 路线逆向工具链）

> 交接文档：如何在酒环境（Arch + wine）里用 IDA Pro 9.4 headless 逆向 libwxhld.dll。
> 状态基准 2026-10-20；配套 README §6.0 是战况，本文只讲工具怎么用。

## 1. 组成

| 组件 | 是什么 | 在哪 |
|---|---|---|
| IDA Pro 9.4 | 商业引擎，含 idalib（无 GUI 分析库） | `/home/banshan/app/ida-pro-9.4` |
| ida-pro-mcp | 社区 MCP 包装（supervisor + HTTP worker） | uv 工具链 `~/.local/share/uv/tools/ida-pro-mcp`，已激活 idalib |
| **分析库 .i64** | libwxhld 全量分析结果（328MB，一次 25 分钟跑出来的） | `~/tmp/wetype-win/extracted/3.0.1.1/libwxhld.i64` |
| idaw.py | 自写 HTTP 客户端（本文主角） | `win/tools/idaw.py` |
| REA MCP | 通用逆向 MCP（已全局注册到 claude/omp/dsh） | `~/.local/lib/mise/.../rea`；经它调 IDA 会超时，见 §5 |

## 2. 快速上手

```sh
# 1) 起 worker（每次会话一次；后台进程会被沙箱清理，死了就重起）
IDADIR=/home/banshan/app/ida-pro-9.4 nohup ~/.local/share/uv/tools/ida-pro-mcp/bin/python \
  -m ida_pro_mcp.idalib_server --host 127.0.0.1 --port 53124 >/dev/null 2>&1 &
sleep 3

# 2) 开库 —— 必须直接开 .i64（开裸 dll = 空库，见 §5 坑1）
cd win/tools
IDAW_URL=http://127.0.0.1:53124/mcp python3 idaw.py idb_open \
  '{"input_path":"/home/banshan/tmp/wetype-win/extracted/3.0.1.1/libwxhld.i64"}'

# 3) 查询（不加 IDAW_URL 时默认就是 53124）
python3 idaw.py lookup_funcs '{"queries":["wxime_create_session"]}'
python3 idaw.py decompile '{"addr":"0x1801e0400"}'
```

## 3. 常用工具速查（`python3 idaw.py <工具> '<JSON参数>'`）

- `--list`：全部 66 个工具及参数
- `lookup_funcs` / `func_query`：按名/地址找函数（含大小）
- `decompile '{"addr":"0x…","include_addresses":true}'`：伪代码。**500 行截断**——大函数（如工厂 0x18021CAC0）只出声明段，要配 disasm 用
- `disasm '{"addr":"0x…","max_instructions":N}'`：汇编。**只回一个基本块**，且 addr 必须是指令边界（给中间地址返回空）——用 `insn_query` 先拿合法边界地址表
- `insn_query '{"queries":[{"start":"0x…","end":"0x…","count":N,"allow_broad":true}]}'`：区间内指令（只回地址，用来探边界）
- `xrefs_to '{"addrs":"0x…"}'`：反查引用。**代码调用回指不完整**（adapter 限制），数据引用（字符串/全局）可靠
- `find_bytes '{"patterns":"48 8B ?? D0"}'`：字节搜索（`??` 通配）
- `find_regex '{"pattern":"wxime_config","limit":30}'`：字符串搜索
- `get_bytes` / `get_string`：读数据
- `idb_save '{"path":"…/libwxhld.i64"}'`：保存分析（换了 worker 后必须重存）

## 4. 典型工作流

1. **找函数**：`lookup_funcs`（wxime_* 导出名）或 `find_regex` 搜日志字符串（PDB 泄漏路径 `E:\data\landun\...` 极有用：日志串直接给出函数名+行号）
2. **读逻辑**：小函数 `decompile`；大函数 `disasm` 逐块走（先 `insn_query` 拿边界）
3. **追调用链**：`xrefs_to`（数据可靠）；代码边用 disasm 里的 `call` 目标人工跟
4. **动态验证**：dbg2.c 断点探针（`run_probe_test.sh dbg2.exe`）抓运行时真值，与静态互相印证

## 5. 坑清单（血泪）

1. **开库必须传 .i64**：传裸 dll 会"成功"打开但反编译只见 `JUMPOUT` 空壳——分析没加载
2. **worker 会被沙箱杀**：每个 bash 调用的进程树独立，worker 跨调用会消失；发现连接拒绝就重起（§2 第 1 步）
3. **REA 的 MCP 层频繁超时**（客户端 60s 限制 vs 冷分析 25 分钟）——直连 worker（idaw.py）是正解；worker 活着时还能跨 REA 抢救分析成果（直连 HTTP 调 `idb_save`）
4. **disasm 基本块限制**：见 §3；另外返回可能是**多帧 JSON**（一行一个对象），解析要逐行取含 `asm` 的帧
5. **地址换算**：IDA 里 libwxhld 基址 0x180000000；wine 运行时模块基址随机，崩溃回溯里的地址要减去运行时基址换 RVA 再加 0x180000000
6. **wetype_server.exe 是另一套**：.i64 只覆盖 libwxhld；server 的静态资料用 `~/tmp/wetype-win/server_disasm.txt`（86MB objdump 全量），运行时真值用 dbg2.c 断点（0x1401a0fd8=initialize 调用点、0x1401a6633=create_session 调用点，server 无 ASLR 基址 0x140000000）
7. **单实例互斥体**：dbg* 探针调试时绝不能预起 wetype_server（run_probe_test.sh 已对 dbg* 自动跳过）
8. clang PE 探针没有 SEH：崩溃捕获用 VEH（probe2.c crash_veh 是模板）
9. **★ .i64 会被硬杀的 worker 写坏（internal error 1228）**：worker 持库时被
   SIGKILL/沙箱清理，.i64 落半写态，之后 idb_open 即 FATAL。2026-10-20 主库
   libwxhld.i64 与备用 libwxhld.dll.i64 先后损坏；恢复用 `win/tools/reanalyze.py`
   从 dll 重跑全量（~25 分钟）。期间静态分析用
   `objdump -d -M intel --start/stop-address=… libwxhld.dll` 直查（指令流完整），
   全量 objdump 存 /tmp 再 grep 调用关系同样高效（server 已有 86MB 全量先例）。
10. **批查用 `win/tools/idaw_batch.py`**（自带 worker 生命周期 + 断线重试），比手动
    起 worker + idaw.py 稳；改探针 C 源别用 sed/python 补丁链——直接整文件重写。

## 6. 当前活跃线索（接手从这继续）

主 README §6.0 条目 14-15（2026-10-21）：**路线 C 打字→全量候选闭环已达成**
（probe3.c：initialize→config_dict→create_session→process_input→listener→
get_candidates→候选解码；`n`→726 个候选、`nihao`→125 个，与 Route W 管道
时代完全一致，全程不崩）。迭代协议已反汇编全破译（回调返回 BOOL 非零=继续；
见 README 条目 14）。**选词已通**（README 条目 15）；**工程化已启动**：
win/wxhost/ 模块化宿主（abi/engine/event/main 四模块，E2E 已通，README
条目 16）——probe3.c 冻结为实证参考，新功能进 wxhost。下一站 =
行协议接入 + 崩溃止崩 + Rime 插件评估。
期间静态分析用 objdump（指南坑 9）；改 C 源只用 Edit 工具（坑 10，
python 补丁链曾把 probe3.c 写爆成 20MB——恢复靠 transcript 重放）。
