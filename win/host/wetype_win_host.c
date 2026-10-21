/* wetype_win_host.c —— vtype wine 宿主守护进程（Route W 核心）
 *
 * 职责：把 vtype 的 stdin/stdout 行协议（PING/OPT/B/L/S/C/R/SAVE/HK/Q，
 * 与 harness/jinterop.c 的 daemon 契约一致）翻译成 wetype_server 的
 * session_v2 命名管道 protobuf 协议（win/protos/commands.proto）。
 *
 * 关键事实（win/README.md §5，实验实证）：
 *   - application_info.process_id 必须等于 GetNamedPipeClientProcessId(hPipe)，
 *     即连接进程自身的 GetCurrentProcessId()——本进程直连管道，天然满足。
 *   - 帧格式：message-type 管道，一次 WriteFile=一条消息，无帧头。
 *   - 响应是异步推送：pendiUpdate(type=3) / candidateUpdate(type=2) / emit(type=1)。
 *
 * 协议映射：
 *   B <keys>  → 逐键 SEND_KEY(key_code=字符, KEY_DOWN) + 等 candidateUpdate（150ms 静默窗）
 *              → CAND\t词1\t词2... / EMPTY
 *   L <key>   → 同 B（单键）
 *   S <idx>   → SELECT_CANDIDATE(int_value=idx) → OK（emit 异步，不等满）
 *   C         → ESC 键清空组词 → OK
 *   PING→PONG  SAVE→OK  Q→退出
 *   OPT spans/tokens → ERR（插件自动降级到基础模式）
 *   R → UNCHANGED（无云）  HK → ERR（常用语码暂不支持）
 *
 * 构建：zig cc -target x86_64-windows-gnu -O2 -o wetype_win_host.exe wetype_win_host.c
 * 运行：wine wetype_win_host.exe   （引擎目录内，wetype_server 需已常驻）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

#define MAX_CAND   72
#define MAX_WORD   256
#define MAX_ID     192     /* 候选 id blob 实测 ~100B，留余量 */
#define POLL_MS    10
#define QUIET_MS   100      /* 候选静默窗（原 150：有新代数后静默这么久即取
                             * 最终表；wxhost 侧实测 100ms 足够合并连发代数，
                             * 每键固定省 50ms 感知延迟） */
#define NOEV_MS    600      /* 无新代数早退窗：server 对部分键（a/o 补全形）
                             * 不推 candidateUpdate，等满总超时 2500ms 是卡顿
                             * 元凶（wxhost 侧同症同方）。有旧快照则沿用回发
                             * （候选不闪没），无则 EMPTY。 */
#define WAIT_CAND  2500     /* B 命令候选总超时 */
#define WAIT_EMIT  1200     /* 选词后等 emit 的超时 */

/* ---------------- 全局状态 ---------------- */
static unsigned g_pid;
static CRITICAL_SECTION g_cs;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static volatile LONG g_pipe_dead;

static unsigned long long g_cand_gen;          /* candidateUpdate 代数 */
static char g_cand[MAX_CAND][MAX_WORD];
static unsigned char g_cand_id[MAX_CAND][MAX_ID];
static int  g_cand_idlen[MAX_CAND];
static int  g_cand_n;

/* 下发给插件的候选快照（B 的应答表 = S 的索引基准） */
static char g_snap[MAX_CAND][MAX_WORD];
static unsigned char g_snap_id[MAX_CAND][MAX_ID];
static int  g_snap_idlen[MAX_CAND];
static int  g_snap_n;

static unsigned long long g_emit_gen;          /* emit 代数 */
static char g_emit[512];

/* ---------------- protobuf 编码 ---------------- */
static int pb_varint(unsigned char *p, unsigned long long v)
{
    int n = 0;
    do { unsigned char b = v & 0x7f; v >>= 7; if (v) b |= 0x80; p[n++] = b; } while (v);
    return n;
}
static int pb_tag(unsigned char *p, int field, int wire)
{
    return pb_varint(p, ((unsigned long long)field << 3) | wire);
}
static int pb_vfield(unsigned char *p, int field, unsigned long long v)
{
    int n = pb_tag(p, field, 0);
    return n + pb_varint(p + n, v);
}
static int pb_bfield(unsigned char *p, int field, const void *d, int len)
{
    int n = pb_tag(p, field, 2);
    n += pb_varint(p + n, (unsigned long long)len);
    if (len > 0) memcpy(p + n, d, len);
    return n + len;
}

/* CommandType（CommandV1.Detail.CommandType）：SEND_KEY=1 SELECT_CANDIDATE=2 CREATE_SESSION=3
 * select_id: 非 NULL 时用 commit=CandidateWord{id}（实验证实的唯一正确姿势，
 * int_value 会让服务器连接线程卡死）。 */
static int build_cmd(unsigned char *out, int command_type, int has_key,
                     unsigned key_code, int has_special, unsigned special_key,
                     const unsigned char *sel_id, int sel_idlen)
{
    unsigned char detail[MAX_ID + 160], appinfo[96], cv1[MAX_ID + 320];
    int nd = 0, na = 0, nc = 0;

    nd += pb_vfield(detail + nd, 1, (unsigned)command_type);
    if (has_key) {
        unsigned char ke[32]; int nk = 0;
        if (key_code) nk += pb_vfield(ke + nk, 1, key_code);       /* key_code */
        if (has_special) nk += pb_vfield(ke + nk, 2, special_key); /* special_key */
        nk += pb_vfield(ke + nk, 3, 8);                            /* KEY_DOWN */
        nd += pb_bfield(detail + nd, 2, ke, nk);
    }
    if (sel_id && sel_idlen > 0) {
        /* Detail.commit(field 5) = CandidateWord{id(field 2)} */
        unsigned char cw[MAX_ID + 16]; int wlen = 0;
        wlen += pb_bfield(cw + wlen, 2, sel_id, sel_idlen);
        nd += pb_bfield(detail + nd, 5, cw, wlen);
    }

    na += pb_vfield(appinfo + na, 1, g_pid);                       /* 真实 PID！ */
    na += pb_vfield(appinfo + na, 2, GetCurrentThreadId());
    na += pb_bfield(appinfo + na, 4, "fcitx5-wetype", 13);
    na += pb_vfield(appinfo + na, 5, 1);                           /* CLIENT_TIP */

    nc += pb_bfield(cv1 + nc, 1, detail, nd);                      /* cmd=[Detail] */
    nc += pb_bfield(cv1 + nc, 2, appinfo, na);                     /* application_info */

    {
        int n = pb_vfield(out, 1, 1);                              /* version=1 */
        n += pb_bfield(out + n, 2, cv1, nc);
        return n;
    }
}

/* ---------------- protobuf 解码 ---------------- */
typedef struct { const unsigned char *p; int len; } span_t;

/* 迭代器：取下一个字段。成功返回 1。*/
static int pb_next(const unsigned char *p, int len, int *pos,
                   int *field, int *wire, span_t *s, unsigned long long *v)
{
    int i = *pos;
    if (i >= len) return 0;
    unsigned long long tag = 0, val = 0;
    int shift = 0, j = i;
    while (j < len) { unsigned char b = p[j++]; tag |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
    if (j >= len && (p[j-1] & 0x80)) return 0;
    *field = (int)(tag >> 3); *wire = (int)(tag & 7);
    s->p = NULL; s->len = 0; *v = 0;
    if (*wire == 0) {
        shift = 0;
        while (j < len) { unsigned char b = p[j++]; val |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
        *v = val;
    } else if (*wire == 2) {
        unsigned long long l = 0; shift = 0;
        while (j < len) { unsigned char b = p[j++]; l |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
        if (j + (int)l > len) return 0;
        s->p = p + j; s->len = (int)l; j += (int)l;
    } else if (*wire == 1) { if (j + 8 > len) return 0; j += 8;
    } else if (*wire == 5) { if (j + 4 > len) return 0; j += 4;
    } else return 0;
    *pos = j;
    return 1;
}

static void span_copy(char *dst, int cap, span_t s)
{
    int L = s.len < cap - 1 ? s.len : cap - 1;
    if (L > 0) memcpy(dst, s.p, L);
    dst[L] = 0;
}

/* CandidateWord{1:string value, 2:bytes id} → 词文本 + 选词凭证 id */
static void cand_word_add(span_t w)
{
    if (g_cand_n >= MAX_CAND) return;
    int pos = 0, field, wire;
    span_t s; unsigned long long v;
    int got_value = 0;
    while (pb_next(w.p, w.len, &pos, &field, &wire, &s, &v)) {
        if (field == 1 && wire == 2) {
            span_copy(g_cand[g_cand_n], MAX_WORD, s);
            got_value = 1;
        } else if (field == 2 && wire == 2) {
            int L = s.len < MAX_ID ? s.len : MAX_ID;
            if (L > 0) memcpy(g_cand_id[g_cand_n], s.p, L);
            g_cand_idlen[g_cand_n] = L;
        }
    }
    if (got_value) g_cand_n++;
}

/* CandidateList{1:repeated CandidateWord} */
static void cand_list_parse(span_t cl)
{
    int pos = 0, field, wire;
    span_t s; unsigned long long v;
    while (pb_next(cl.p, cl.len, &pos, &field, &wire, &s, &v)) {
        if (field == 1 && wire == 2) cand_word_add(s);
    }
}

/* Detail{1:int32 type, 2:CandidateList, 3:PendingList, 4:string emit_str} */
static void handle_detail(span_t d)
{
    int pos = 0, field, wire;
    span_t s; unsigned long long v;
    unsigned long long type = 0;
    span_t cand = { NULL, 0 }, emit = { NULL, 0 };
    while (pb_next(d.p, d.len, &pos, &field, &wire, &s, &v)) {
        if (field == 1 && wire == 0) type = v;
        else if (field == 2 && wire == 2) cand = s;
        else if (field == 4 && wire == 2) emit = s;
    }
    EnterCriticalSection(&g_cs);
    if (type == 1) {            /* emit：选词上屏文本 */
        g_emit_gen++;
        g_emit[0] = 0;
        if (emit.p) span_copy(g_emit, sizeof g_emit, emit);
        else if (cand.p) span_copy(g_emit, sizeof g_emit, cand);  /* 文本可能落在其他字段 */
        LeaveCriticalSection(&g_cs);
        fprintf(stderr, "[win-host] emit: %s\n", g_emit);
    } else if (type == 2) {     /* candidateUpdate：候选列表 */
        g_cand_gen++;
        g_cand_n = 0;
        if (cand.p) cand_list_parse(cand);
        int n = g_cand_n;
        LeaveCriticalSection(&g_cs);
        fprintf(stderr, "[win-host] candidates gen=%llu n=%d first=%s\n",
                (unsigned long long)g_cand_gen, n, n > 0 ? g_cand[0] : "");
    } else {
        LeaveCriticalSection(&g_cs);   /* pendiUpdate(3)/configUpdate(4) 等忽略 */
    }
}

/* 解析一条 RespBase 消息，更新共享状态 */
static void parse_message(const unsigned char *msg, int len)
{
    int pos = 0, field, wire;
    span_t s; unsigned long long v;
    /* RespBase{2:bytes resp} */
    while (pb_next(msg, len, &pos, &field, &wire, &s, &v)) {
        if (field != 2 || wire != 2 || !s.p) continue;
        /* RespBodyV1{2:repeated Detail} */
        int pos2 = 0, field2, wire2;
        span_t s2; unsigned long long v2;
        while (pb_next(s.p, s.len, &pos2, &field2, &wire2, &s2, &v2)) {
            if (field2 == 2 && wire2 == 2) handle_detail(s2);
        }
    }
}

int main(int argc, char **argv);

/* 读线程：循环读管道消息 → 解析 → 更新状态。
 * 句柄生命周期只归主线程（ensure_pipe 关旧开新）；本线程仅读快照，
 * 失败就置 dead 标志并让出（等主线程在下条命令时重连）。 */
static DWORD WINAPI reader_thread(void *arg)
{
    (void)arg;
    unsigned char buf[65536];
    DWORD n = 0;
    for (;;) {
        HANDLE h = g_pipe;
        if (h == INVALID_HANDLE_VALUE || g_pipe_dead) { Sleep(100); continue; }
        if (!ReadFile(h, buf, sizeof buf, &n, NULL) || n == 0) {
            InterlockedExchange(&g_pipe_dead, 1);
            fprintf(stderr, "[win-host] pipe read lost (err=%lu), will reconnect\n",
                    GetLastError());
            continue;
        }
        parse_message(buf, (int)n);
    }
    return 0;
}

/* ---------------- 管道连接 ---------------- */
static volatile LONG g_write_busy;      /* 1 = 主线程有 WriteFile 在途 */
static HANDLE g_write_done;             /* 事件：写完成 */

/* 写看门狗：服务器连接被楔死时 WriteFile 永久阻塞（int_value 实验实证过），
 * 3s 未完成就 CancelIoEx 掐断该句柄所有 IO → 主/读线程各自退出 → 重连自愈。 */
static DWORD WINAPI write_watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        Sleep(200);
        HANDLE h = g_pipe;
        if (h == INVALID_HANDLE_VALUE || !g_write_busy) continue;
        if (WaitForSingleObject(g_write_done, 3000) == WAIT_OBJECT_0) continue;
        h = g_pipe;                       /* 期间句柄可能已换新，重取再判 */
        if (h != INVALID_HANDLE_VALUE && g_write_busy) {
            fprintf(stderr, "[win-host] write stuck 3s, cancel IO & reconnect\n");
            InterlockedExchange(&g_pipe_dead, 1);
            CancelIoEx(h, NULL);
        }
    }
    return 0;
}

static int pipe_send(const unsigned char *msg, int len)
{
    DWORD n = 0;
    if (g_pipe == INVALID_HANDLE_VALUE) return -1;
    ResetEvent(g_write_done);
    InterlockedExchange(&g_write_busy, 1);
    BOOL ok = WriteFile(g_pipe, msg, len, &n, NULL) && n == (DWORD)len;
    InterlockedExchange(&g_write_busy, 0);
    SetEvent(g_write_done);
    if (!ok) {
        InterlockedExchange(&g_pipe_dead, 1);
        return -1;
    }
    FlushFileBuffers(g_pipe);
    return 0;
}

/* 引擎不在（无管道）时自己拉起 wetype_server.exe：cwd=本 exe 所在目录
 * （BuildIn 词库是相对路径），环境继承（WINEPREFIX 由上游设好）。 */
static void spawn_server(void)
{
    wchar_t self[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(dir, self);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    _snwprintf(exe, MAX_PATH, L"%s\\wetype_server.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[win-host] wetype_server.exe not found next to host\n");
        return;
    }
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    wchar_t cmd[] = L"wetype_server.exe logtostderr";
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, dir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        fprintf(stderr, "[win-host] spawned wetype_server.exe\n");
    } else {
        fprintf(stderr, "[win-host] spawn wetype_server.exe failed err=%lu\n",
                GetLastError());
    }
}

/* 管道名：默认 key 随发行树稳定（实测与安装路径无关，838180e9… 为
 * 3.0.1.1 引擎树+配套前缀的 key）；WETYPE_WIN_PIPE 可整名覆盖。
 * 注：wine 不支持 \\.\pipe 枚举（FindFirstFile 报 66、NT 目录对象不存在），
 * 因此无法运行时自动发现，只能约定或配置。 */
static const char *pipe_name(void)
{
    const char *env = getenv("WETYPE_WIN_PIPE");
    if (env && *env) return env;
    return "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2";
}

static int ensure_pipe(void)
{
    if (g_pipe != INVALID_HANDLE_VALUE && !g_pipe_dead) return 1;
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_pipe, NULL);        /* 掐掉读线程挂着的 ReadFile 再关 */
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    wchar_t wname[512];
    MultiByteToWideChar(CP_UTF8, 0, pipe_name(), -1, wname, 512);
    g_pipe = CreateFileW(wname, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);
        InterlockedExchange(&g_pipe_dead, 0);
        unsigned char req[MAX_ID + 160];
        pipe_send(req, build_cmd(req, 3 /*CREATE_SESSION*/, 0, 0, 0, 0, NULL, 0));
        return 1;
    }
    return 0;
}

/* stdin 是否有待读数据（插件连接的是管道 → PeekNamedPipe 可用；终端等非管道返回 0）。
 * 快速打字时插件逐键排队 B，若每条都等满 150ms 静默窗，延迟逐键叠加（卡顿）。
 * 有新命令排队就提前截断静默窗，最新候选立刻回，下条 B 再刷新。 */
static int stdin_pending(void)
{
    static HANDLE hstdin = NULL;
    if (!hstdin) hstdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (!PeekNamedPipe(hstdin, NULL, 0, NULL, &avail, NULL)) return 0;
    return avail > 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    InitializeCriticalSection(&g_cs);
    g_pid = GetCurrentProcessId();

    setvbuf(stdout, NULL, _IONBF, 0);
    /* 关键：mingw PE 默认文本模式会把 \n 翻成 \r\n，插件按行精确匹配
     * （line == "READY"）会因尾部 \r 失败 → 启动看门狗杀引擎。必须二进制。 */
    _setmode(_fileno(stdout), _O_BINARY);
    fprintf(stderr, "[win-host] starting pid=%u\n", g_pid);

    /* 等管道就绪：先试 2s（服务器可能已在跑），不行就拉起引擎，共 60s */
    int ok = 0;
    for (int i = 0; i < 120 && !(ok = ensure_pipe()); i++) {
        if (i == 4) spawn_server();
        Sleep(500);
    }
    if (!ok) { printf("ERR\n"); fprintf(stderr, "[win-host] pipe not found\n"); return 1; }
    fprintf(stderr, "[win-host] pipe connected\n");

    CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    g_write_done = CreateEventW(NULL, FALSE, TRUE, NULL);   /* 自动复位，初态有信号 */
    CreateThread(NULL, 0, write_watchdog, NULL, 0, NULL);
    printf("READY\n");

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;
        fprintf(stderr, "[win-host] cmd=%c len=%zu\n", line[0], L);

        if (!strcmp(line, "Q")) break;
        if (!strcmp(line, "PING")) { printf("PONG\n"); continue; }
        if (!strcmp(line, "OPT spans") || !strcmp(line, "OPT tokens")) { printf("ERR\n"); continue; }
        if (!strcmp(line, "SAVE")) { printf("OK\n"); continue; }
        if (!strcmp(line, "R"))    { printf("UNCHANGED\n"); continue; }
        if (line[0] == 'H' && line[1] == 'K') { printf("ERR\n"); continue; }

        if (!strcmp(line, "C")) {
            /* 清空组词：实测 ESC 清不掉"候选展示中"的会话状态（组词冻死、按键被吞），
             * 唯一可靠的重置 = 断开重连 + CREATE_SESSION（新连接 = 干净会话，
             * 与启动路径同一条被验证的路）。ensure_pipe 内部会自动重建会话。 */
            InterlockedExchange(&g_pipe_dead, 1);
            g_snap_n = 0;   /* 新会话：旧快照 id 凭证作废，禁用沿用回发 */
            if (!ensure_pipe()) {
                /* 重连失败也要按契约回 OK（插件期望 C 有应答）；
                 * 下条命令的 ensure_pipe 会再试。 */
                fprintf(stderr, "[win-host] C 重连暂失败，稍后由下条命令重试\n");
            }
            printf("OK\n");
            continue;
        }

        if ((line[0] == 'B' || line[0] == 'L') && line[1] == ' ') {
            const char *keys = line + 2;
            if (!*keys) { printf("EMPTY\n"); continue; }
            if (!ensure_pipe()) { printf("ERR\n"); continue; }
            int dead = 0;
            for (const char *k = keys; *k; ++k) {
                unsigned char req[96];
                if (pipe_send(req, build_cmd(req, 1 /*SEND_KEY*/, 1,
                                              (unsigned char)*k, 0, 0, NULL, 0)) < 0) { dead = 1; break; }
                if (line[0] == 'L') break;
            }
            if (dead) { printf("EMPTY\n"); continue; }

            /* 等候选：新代数出现后 QUIET_MS 静默即取，总超时 WAIT_CAND。
             * 快照同时存 id——S 选词必须用「插件看到的那份表」，云候选晚到
             * 覆盖活表也不能改变已下发的索引语义。 */
            unsigned long long seen;
            EnterCriticalSection(&g_cs); seen = g_cand_gen; LeaveCriticalSection(&g_cs);
            int have = 0;
            ULONGLONG t0 = GetTickCount64(), t_last = 0;
            while (GetTickCount64() - t0 < (ULONGLONG)WAIT_CAND) {
                Sleep(POLL_MS);
                int done = 0;
                EnterCriticalSection(&g_cs);
                if (g_cand_gen > seen) {
                    seen = g_cand_gen;
                    g_snap_n = g_cand_n;
                    for (int i = 0; i < g_snap_n && i < MAX_CAND; i++) {
                        memcpy(g_snap[i], g_cand[i], MAX_WORD);
                        memcpy(g_snap_id[i], g_cand_id[i], MAX_ID);
                        g_snap_idlen[i] = g_cand_idlen[i];
                    }
                    have = 1; t_last = GetTickCount64();
                }
                /* 新输入在排队 → 不再等静默窗，立刻回最新候选（治卡顿） */
                if (have && stdin_pending()) done = 1;
                if (have && GetTickCount64() - t_last >= (ULONGLONG)QUIET_MS) done = 1;
                /* 无新代数早退：server 不推（补全键语义=候选不变）——有旧
                 * 快照沿用回发（候选窗不闪没），避免等满 2500ms 卡顿 */
                if (!have && GetTickCount64() - t0 >= (ULONGLONG)NOEV_MS) done = 1;
                LeaveCriticalSection(&g_cs);
                if (done) break;
            }
            if (have && g_snap_n > 0) {
                printf("CAND");
                for (int i = 0; i < g_snap_n; i++) printf("\t%s", g_snap[i]);
                printf("\n");
            } else if (g_snap_n > 0) {
                fprintf(stderr, "[win-host] B 无新代数，沿用快照 %d 个\n", g_snap_n);
                printf("CAND");
                for (int i = 0; i < g_snap_n; i++) printf("\t%s", g_snap[i]);
                printf("\n");
            } else {
                printf("EMPTY\n");
            }
            continue;
        }

        if (line[0] == 'S') {
            int idx = atoi(line + 1);
            if (!ensure_pipe()) { printf("ERR\n"); continue; }
            /* 取该候选的 id 凭证（快照表，与下发 CAND 一致） */
            unsigned char sel_id[MAX_ID];
            int sel_idlen = 0;
            if (idx >= 0 && idx < g_snap_n && g_snap_idlen[idx] > 0) {
                sel_idlen = g_snap_idlen[idx];
                memcpy(sel_id, g_snap_id[idx], sel_idlen);
            }
            if (sel_idlen == 0) {
                fprintf(stderr, "[win-host] S idx=%d 无凭证(快照n=%d) → ERR\n", idx, g_snap_n);
                printf("ERR\n");
                continue;
            }

            unsigned char req[MAX_ID + 160];
            unsigned long long seen_emit, seen_cand;
            EnterCriticalSection(&g_cs);
            seen_emit = g_emit_gen;
            seen_cand = g_cand_gen;
            LeaveCriticalSection(&g_cs);
            if (pipe_send(req, build_cmd(req, 2 /*SELECT_CANDIDATE*/, 0, 0, 0, 0,
                                          sel_id, sel_idlen)) < 0) { printf("ERR\n"); continue; }
            fprintf(stderr, "[win-host] S idx=%d idlen=%d 已发\n", idx, sel_idlen);
            /* 1) 等 emit：选词被引擎受理的标志；等不到 = 服务器没接受，回 ERR
             *    （千万别学旧版无脑回 OK——插件会当整段完成清缓冲，引擎剩组词错位）。 */
            int got_emit = 0;
            ULONGLONG t0 = GetTickCount64();
            while (GetTickCount64() - t0 < (ULONGLONG)WAIT_EMIT) {
                Sleep(POLL_MS);
                EnterCriticalSection(&g_cs);
                if (g_emit_gen > seen_emit) got_emit = 1;
                LeaveCriticalSection(&g_cs);
                if (got_emit) break;
            }
            if (!got_emit) {
                fprintf(stderr, "[win-host] S idx=%d 无 emit → ERR\n", idx);
                printf("ERR\n");
                continue;
            }
            /* 2) 部分选词：emit 后引擎若剩组词（如 nihao 选「你」剩 hao）会再推
             *    candidateUpdate。契约（jinterop）：S 回 CAND=剩余候选继续组词，
             *    回 OK=整段完成。剩余候选同时成为新快照（后续 S 的索引基准）。 */
            int n_left = -1;
            ULONGLONG t1 = GetTickCount64();
            while (GetTickCount64() - t1 < 600) {
                Sleep(POLL_MS);
                EnterCriticalSection(&g_cs);
                if (g_cand_gen > seen_cand) {
                    seen_cand = g_cand_gen;
                    g_snap_n = g_cand_n;
                    for (int i = 0; i < g_snap_n && i < MAX_CAND; i++) {
                        memcpy(g_snap[i], g_cand[i], MAX_WORD);
                        memcpy(g_snap_id[i], g_cand_id[i], MAX_ID);
                        g_snap_idlen[i] = g_cand_idlen[i];
                    }
                    n_left = g_snap_n;
                }
                LeaveCriticalSection(&g_cs);
                if (n_left >= 0 && GetTickCount64() - t1 >= (ULONGLONG)QUIET_MS) break;
            }
            if (n_left > 0) {
                fprintf(stderr, "[win-host] S 部分选词，剩余 %d 候选（首=%s）\n",
                        n_left, g_snap[0]);
                printf("CAND");
                for (int i = 0; i < n_left; i++) printf("\t%s", g_snap[i]);
                printf("\n");
            } else {
                fprintf(stderr, "[win-host] S idx=%d 整段完成(emit=%s)\n", idx, g_emit);
                printf("OK\n");
            }
            continue;
        }

        printf("ERR\n");
    }
    return 0;
}
