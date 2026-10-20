/* 连 wetype session_v2 管道，探测握手协议 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void dump(const char *tag, const unsigned char *b, DWORD n)
{
    printf("%s[%lu]:", tag, n);
    for (DWORD i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n"); fflush(stdout);
}

static int read_avail(HANDLE h, unsigned char *buf, DWORD cap, const char *tag)
{
    DWORD avail = 0, left = 0, nread = 0;
    if (!PeekNamedPipe(h, buf, cap, &nread, &avail, &left) || avail == 0) {
        printf("(%s: 无数据)\n", tag); fflush(stdout);
        return 0;
    }
    if (!ReadFile(h, buf, cap, &nread, NULL)) return 0;
    dump(tag, buf, nread);
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = argv[1] ? argv[1] : "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2";
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 512);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("open failed %lu\n", GetLastError()); return 1; }
    printf("connected: %s\n", name); fflush(stdout);

    unsigned char buf[4096];
    DWORD n = 0;

    /* 1) 先听：服务器连接后是否主动发东西 */
    Sleep(1000);
    read_avail(h, buf, sizeof buf, "server-first");

    /* 2) 发探针：全零 4 字节 */
    unsigned char probe[4] = {0, 0, 0, 0};
    WriteFile(h, probe, 4, &n, NULL);
    printf("sent 4 zero bytes (n=%lu)\n", n); fflush(stdout);
    Sleep(1200);
    read_avail(h, buf, sizeof buf, "resp-to-zero");

    /* 3) 发探针：mozc 风格 magic 0x1fbe + 长度 + 载荷 */
    unsigned char probe2[8] = {0x1f, 0xbe, 0, 4, 0xde, 0xad, 0xbe, 0xef};
    WriteFile(h, probe2, 8, &n, NULL);
    printf("sent magic probe (n=%lu)\n", n); fflush(stdout);
    Sleep(1200);
    read_avail(h, buf, sizeof buf, "resp-to-magic");

    CloseHandle(h);
    return 0;
}
