/* session_probe.c —— Phase1 闭环验证探针（vtype-linux win 路线）
 *
 * 目的：验证 PID 强校验假设（见 win/README.md §5）：
 *   探针【自身】连接 session_v2 管道，application_info.process_id 填
 *   GetCurrentProcessId() 的真实值，服务端应正常回包。
 *
 * 流程：
 *   1. CreateFileW 连管道（本进程即管道客户端 → PID 天然匹配）
 *   2. 读服务端 hello 推送（202B RespBase）
 *   3. 发 CREATE_SESSION（Detail.command_type=3），读回包
 *   4. 逐键发 SEND_KEY 'n' 'i' 'h' 'a' 'o'（key_code + KEY_DOWN），读回包
 *   5. 全部消息 hexdump 到 stdout，便于 Linux 侧离线 protobuf 解码
 *
 * protobuf 手工编码，字段号对照 win/protos/commands.proto（3.0.1.1）：
 *   CommandBase{version=1:int32, cmd=2:bytes}
 *   CommandV1{cmd=1:repeated Detail, application_info=2}
 *   Detail{command_type=1:int32, key=2:KeyEvent, int_value=3}
 *   KeyEvent{key_code=1:uint32, special_key=2, modifier_keys=3:repeated}
 *   ApplicationInfo{process_id=1:uint32, thread_id=2, process_name=4, client_type=5}
 *   CommandType: SEND_KEY=1 SELECT_CANDIDATE=2 CREATE_SESSION=3
 *   ModifierKey: KEY_DOWN=8
 *
 * 构建：zig cc -target x86_64-windows-gnu -O1 -o session_probe.exe session_probe.c
 * 运行：cp 到引擎目录后 wine session_probe.exe [管道名]
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define DEFAULT_PIPE "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2"
#define POLL_MS 25

static HANDLE g_pipe;

/* ---------- protobuf 编码 ---------- */
static int put_varint(unsigned char *p, unsigned long long v)
{
    int n = 0;
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        p[n++] = b;
    } while (v);
    return n;
}
static int put_tag(unsigned char *p, int field, int wire)
{
    return put_varint(p, ((unsigned long long)field << 3) | wire);
}
static int put_varint_field(unsigned char *p, int field, unsigned long long v)
{
    int n = put_tag(p, field, 0);
    n += put_varint(p + n, v);
    return n;
}
static int put_bytes_field(unsigned char *p, int field, const void *data, int len)
{
    int n = put_tag(p, field, 2);
    n += put_varint(p + n, (unsigned long long)len);
    if (len > 0) memcpy(p + n, data, len);
    return n + len;
}

/* 构造完整 CommandBase。has_key=0 时省略 KeyEvent（用于 CREATE_SESSION 等）。 */
static int build_command(unsigned char *out, int command_type, int has_key,
                         unsigned key_code, unsigned pid, unsigned tid,
                         int has_int, int int_value)
{
    unsigned char detail[96], appinfo[96], cv1[224];
    int nd = 0, na = 0, nc = 0;

    nd += put_varint_field(detail + nd, 1, (unsigned)command_type);
    if (has_key) {
        unsigned char ke[32]; int nk = 0;
        nk += put_varint_field(ke + nk, 1, key_code);   /* key_code */
        nk += put_varint_field(ke + nk, 3, 8);          /* modifier_keys=[KEY_DOWN] */
        nd += put_bytes_field(detail + nd, 2, ke, nk);  /* key */
    }
    if (has_int)
        nd += put_varint_field(detail + nd, 3, (unsigned)int_value);

    na += put_varint_field(appinfo + na, 1, pid);       /* process_id：真实 PID！ */
    na += put_varint_field(appinfo + na, 2, tid);       /* thread_id */
    na += put_bytes_field(appinfo + na, 4, "wetype_probe", 12);
    na += put_varint_field(appinfo + na, 5, 1);         /* client_type=CLIENT_TIP */

    nc += put_bytes_field(cv1 + nc, 1, detail, nd);     /* cmd=[Detail] */
    nc += put_bytes_field(cv1 + nc, 2, appinfo, na);    /* application_info */

    {
        int n = put_varint_field(out, 1, 1);            /* version=1 */
        n += put_bytes_field(out + n, 2, cv1, nc);      /* cmd=CommandV1 */
        return n;
    }
}

/* SELECT_CANDIDATE 变体A：int_value=idx（0基?） */
static int build_select_int(unsigned char *out, int idx, unsigned pid, unsigned tid)
{
    unsigned char detail[32], appinfo[96], cv1[160];
    int nd = 0, na = 0, nc = 0;
    nd += put_varint_field(detail + nd, 1, 2 /*SELECT_CANDIDATE*/);
    nd += put_varint_field(detail + nd, 3, (unsigned)idx);
    na += put_varint_field(appinfo + na, 1, pid);
    na += put_varint_field(appinfo + na, 2, tid);
    na += put_bytes_field(appinfo + na, 4, "wetype_probe", 12);
    na += put_varint_field(appinfo + na, 5, 1);
    nc += put_bytes_field(cv1 + nc, 1, detail, nd);
    nc += put_bytes_field(cv1 + nc, 2, appinfo, na);
    {
        int n = put_varint_field(out, 1, 1);
        n += put_bytes_field(out + n, 2, cv1, nc);
        return n;
    }
}

/* ESC 键：KeyEvent{special_key=10 ESCAPE} + KEY_DOWN，试清空组词 */
static int build_escape(unsigned char *out, unsigned pid, unsigned tid)
{
    unsigned char ke[32], detail[48], appinfo[96], cv1[160];
    int nk = 0, nd = 0, na = 0, nc = 0;
    nk += put_varint_field(ke + nk, 2, 10 /*ESCAPE*/);
    nk += put_varint_field(ke + nk, 3, 8 /*KEY_DOWN*/);
    nd += put_varint_field(detail + nd, 1, 1 /*SEND_KEY*/);
    nd += put_bytes_field(detail + nd, 2, ke, nk);
    na += put_varint_field(appinfo + na, 1, pid);
    na += put_varint_field(appinfo + na, 2, tid);
    na += put_bytes_field(appinfo + na, 4, "wetype_probe", 12);
    na += put_varint_field(appinfo + na, 5, 1);
    nc += put_bytes_field(cv1 + nc, 1, detail, nd);
    nc += put_bytes_field(cv1 + nc, 2, appinfo, na);
    {
        int n = put_varint_field(out, 1, 1);
        n += put_bytes_field(out + n, 2, cv1, nc);
        return n;
    }
}

/* ---------- 管道 IO ---------- */
static void hexdump(const char *tag, const unsigned char *buf, DWORD n)
{
    printf("%s[%lu]:", tag, n);
    for (DWORD i = 0; i < n; i++) {
        if (i % 32 == 0) printf("\n  %04lx:", i);
        printf(" %02X", buf[i]);
    }
    printf("\n");
    fflush(stdout);
}

/* 轮询读一条消息；0=成功 1=超时 -1=错误 */
static int pipe_read_msg(unsigned char *buf, DWORD cap, DWORD *out, int timeout_ms)
{
    DWORD n = 0, avail = 0;
    for (int waited = 0; waited < timeout_ms; waited += POLL_MS) {
        if (PeekNamedPipe(g_pipe, buf, cap, &n, &avail, NULL) && avail > 0) {
            if (!ReadFile(g_pipe, buf, cap, out, NULL)) {
                printf("[read err %lu]\n", GetLastError());
                return -1;
            }
            return 0;
        }
        Sleep(POLL_MS);
    }
    return 1;
}

static int pipe_send(const unsigned char *msg, int len)
{
    DWORD n = 0;
    if (!WriteFile(g_pipe, msg, len, &n, NULL) || n != (DWORD)len) {
        printf("[write err %lu]\n", GetLastError());
        return -1;
    }
    FlushFileBuffers(g_pipe);
    return 0;
}

int main(int argc, char **argv)
{
    const char *pipename = argc > 1 ? argv[1] : DEFAULT_PIPE;
    unsigned char req[256], buf[65536];
    DWORD got = 0;
    int n;

    setvbuf(stdout, NULL, _IONBF, 0);

    wchar_t wname[512];
    MultiByteToWideChar(CP_UTF8, 0, pipename, -1, wname, 512);
    g_pipe = CreateFileW(wname, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        printf("pipe open failed %lu\n", GetLastError());
        return 1;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);

    unsigned pid = GetCurrentProcessId();
    unsigned tid = GetCurrentThreadId();
    printf("connected. real pid=%u tid=%u\n", pid, tid);

    /* 1) 吃 hello 推送 */
    if (pipe_read_msg(buf, sizeof buf, &got, 3000) == 0)
        hexdump("[hello]", buf, got);
    else
        printf("[no hello push — continue anyway]\n");

    /* 2) CREATE_SESSION */
    n = build_command(req, 3 /*CREATE_SESSION*/, 0, 0, pid, tid, 0, 0);
    hexdump("[CREATE_SESSION req]", req, n);
    if (pipe_send(req, n) < 0) return 2;
    if (pipe_read_msg(buf, sizeof buf, &got, 5000) == 0)
        hexdump("[CREATE_SESSION resp]", buf, got);
    else
        printf("[NO resp to CREATE_SESSION]\n");

    /* 3) 逐键 SEND_KEY "nihao" */
    const char *word = "nihao";
    for (const char *k = word; *k; k++) {
        n = build_command(req, 1 /*SEND_KEY*/, 1, (unsigned char)*k, pid, tid, 0, 0);
        if (pipe_send(req, n) < 0) return 3;
        printf("--- key '%c' sent ---\n", *k);
        if (pipe_read_msg(buf, sizeof buf, &got, 4000) == 0)
            hexdump("[key resp]", buf, got);
        else
            printf("[no resp for '%c']\n", *k);
    }

    /* 4) 额外等一会，收异步推送（candidateUpdate 等） */
    for (int i = 0; i < 4; i++) {
        if (pipe_read_msg(buf, sizeof buf, &got, 1000) == 0)
            hexdump("[async]", buf, got);
        else
            break;
    }

    /* 5) 选词实验：SELECT_CANDIDATE int_value=0，期待 emit "你好" */
    n = build_select_int(req, 0, pid, tid);
    if (pipe_send(req, n) < 0) return 4;
    printf("--- SELECT int_value=0 sent ---\n");
    for (int i = 0; i < 2; i++) {
        if (pipe_read_msg(buf, sizeof buf, &got, 1000) == 0)
            hexdump("[select resp]", buf, got);
        else
            break;
    }

    /* 6) ESC 清空实验 */
    n = build_escape(req, pid, tid);
    if (pipe_send(req, n) < 0) return 5;
    printf("--- ESC sent ---\n");
    for (int i = 0; i < 2; i++) {
        if (pipe_read_msg(buf, sizeof buf, &got, 800) == 0)
            hexdump("[esc resp]", buf, got);
        else
            break;
    }

    printf("done\n");
    CloseHandle(g_pipe);
    return 0;
}
