/* WM_COPYDATA 激活实验：向 wetype.server.window 发注册消息，然后试管道命令 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

struct CDS { ULONG_PTR dwData; DWORD cbData; void *lpData; };

int main(int argc, char **argv)
{
    HWND w = FindWindowW(L"wetype.server.window", NULL);
    if (!w) { printf("window not found\n"); return 1; }
    printf("window=%p\n", w); fflush(stdout);

    /* 尝试每种 COPYDATA 类型，8 字节负载 {pid, tid} */
    for (ULONG_PTR type = 0; type <= 22; type++) {
        if (type == 5) continue; /* SEND_CRASH_DUMP 跳过 */
        unsigned long long payload[2] = { GetCurrentProcessId(), GetCurrentThreadId() };
        struct CDS cds = { type, sizeof payload, payload };
        LRESULT r = SendMessageTimeoutW(w, WM_COPYDATA, (WPARAM)w, (LPARAM)&cds,
                                        SMTO_ABORTIFHUNG, 500, NULL);
        printf("type %lu -> %lld\n", (unsigned long)type, (long long)r);
        fflush(stdout);
    }

    /* 现在试管道 */
    wchar_t pn[512];
    MultiByteToWideChar(CP_UTF8, 0, "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2", -1, pn, 512);
    HANDLE h = CreateFileW(pn, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("pipe open fail %lu\n", GetLastError()); return 2; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    /* CommandBase{version:1, cmd:CommandV1{appinfo, cmd[CREATE_SESSION=3]}} */
    unsigned char req[] = {
        0x08, 0x01, 0x12, 0x12, 0x0a, 0x02, 0x08, 0x03, 0x12, 0x0c, 0x08, 0xd2, 0x09,
        0x22, 0x05, 'p', 'r', 'o', 'b', 'e', 0x28, 0x01
    };
    DWORD n = 0;
    /* 先吃 hello */
    Sleep(400);
    unsigned char buf[16384];
    DWORD avail = 0;
    if (PeekNamedPipe(h, buf, sizeof buf, &n, &avail, NULL) && avail)
        ReadFile(h, buf, sizeof buf, &n, NULL), printf("ate push %lu\n", n);
    if (WriteFile(h, req, sizeof req, &n, NULL))
        printf("sent %lu\n", n);
    for (int i = 0; i < 12; i++) {
        Sleep(250);
        if (PeekNamedPipe(h, buf, sizeof buf, &n, &avail, NULL) && avail) {
            ReadFile(h, buf, sizeof buf, &n, NULL);
            printf("PIPE REPLY[%lu]:", n);
            for (DWORD j = 0; j < n && j < 60; j++) printf(" %02X", buf[j]);
            printf("\n"); fflush(stdout);
        }
    }
    printf("done\n");
    return 0;
}
