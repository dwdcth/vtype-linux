/* 消息类型矩阵测试：每种 payload 独立连接试一次 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static const char *PIPE = "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2";

static int try_payload2(const char *tag, const unsigned char *pb, int plen, int ack_mode)
{
    /* ack_mode: 0=直接发; 1=先等首推->发1字节ACK->再发; 2=等首推再直接发 */
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, PIPE, -1, w, 512);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("%s: open fail %lu\n", tag, GetLastError()); return 0; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    DWORD n = 0;
    unsigned char junk[16384]; DWORD got = 0;

    if (ack_mode == 2) { /* 等首推 */
        for (int i = 0; i < 10; i++) {
            Sleep(100);
            DWORD avail = 0;
            if (PeekNamedPipe(h, junk, sizeof junk, &got, &avail, NULL) && avail) {
                ReadFile(h, junk, sizeof junk, &got, NULL);
                printf("%s: ate-push[%lu]\n", tag, got); fflush(stdout);
                break;
            }
        }
    }
    if (ack_mode == 1) {
        /* 等 hello 推 */
        for (int i = 0; i < 10; i++) {
            Sleep(100);
            DWORD avail = 0;
            if (PeekNamedPipe(h, junk, sizeof junk, &got, &avail, NULL) && avail) {
                ReadFile(h, junk, sizeof junk, &got, NULL);
                printf("%s: ate-push[%lu]\n", tag, got); fflush(stdout);
                break;
            }
        }
        unsigned char ack = 0;
        WriteFile(h, &ack, 1, &n, NULL);
        Sleep(200);
    }
    if (!WriteFile(h, pb, plen, &n, NULL)) {
        printf("%s: write fail %lu\n", tag, GetLastError());
        CloseHandle(h); return 0;
    }
    for (int round = 0; round < 15; round++) {
        Sleep(100);
        DWORD avail = 0;
        if (PeekNamedPipe(h, junk, sizeof junk, &got, &avail, NULL) && avail) {
            if (!ReadFile(h, junk, sizeof junk, &got, NULL)) break;
            printf("%s: MSG[%lu]", tag, got);
            for (DWORD i = 0; i < got && i < 64; i++) printf(" %02X", junk[i]);
            printf("%s\n", got > 64 ? " ..." : "");
            fflush(stdout);
        }
    }
    printf("%s: done\n", tag); fflush(stdout);
    CloseHandle(h);
    return 1;
}

static int try_payload(const char *tag, const unsigned char *pb, int plen)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, PIPE, -1, w, 512);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("%s: open fail %lu\n", tag, GetLastError()); return 0; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    DWORD n = 0;
    if (!WriteFile(h, pb, plen, &n, NULL)) {
        printf("%s: write fail %lu\n", tag, GetLastError());
        CloseHandle(h); return 0;
    }
    /* 连读 1.5s，把所有到达的都打出来（包括首推和响应） */
    for (int round = 0; round < 15; round++) {
        Sleep(100);
        unsigned char buf[8192]; DWORD avail = 0, left = 0, got = 0;
        if (PeekNamedPipe(h, buf, sizeof buf, &got, &avail, &left) && avail) {
            if (!ReadFile(h, buf, sizeof buf, &got, NULL)) break;
            printf("%s: MSG[%lu]", tag, got);
            for (DWORD i = 0; i < got && i < 64; i++) printf(" %02X", buf[i]);
            printf("%s\n", got > 64 ? " ..." : "");
            fflush(stdout);
        }
    }
    printf("%s: done\n", tag); fflush(stdout);
    CloseHandle(h);
    return 1;
}

static int hex2bin(const char *hex, unsigned char *out, int cap)
{
    int n = 0;
    for (const char *p = hex; *p && n < cap; ) {
        unsigned v; if (sscanf(p, "%2x", &v) != 1) break;
        out[n++] = v; p += 2;
    }
    return n;
}

int main(int argc, char **argv)
{
    unsigned char buf[4096];
    for (int i = 1; i < argc; i += 2) {
        int n = hex2bin(argv[i + 1], buf, sizeof buf);
        printf("== %s (%d bytes)\n", argv[i], n);
        int am = argv[i][0] == 'A' ? 1 : (argv[i][0] == 'W' ? 2 : 0);
        try_payload2(argv[i], buf, n, am);
    }
    return 0;
}
