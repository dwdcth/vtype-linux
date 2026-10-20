/* 管道桥：wetype session 管道 <-> stdin/stdout，协议逻辑留在宿主侧 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HANDLE g_pipe;

static DWORD WINAPI pipe_to_stdout(void *arg)
{
    (void)arg;
    unsigned char buf[65536];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(g_pipe, buf, sizeof buf, &n, NULL) || n == 0) break;
        DWORD w = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, NULL) || w != n) break;
        FlushFileBuffers(GetStdHandle(STD_OUTPUT_HANDLE));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: pipebridge <\\\\.\\pipe\\name>\n"); return 2; }
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, w, 512);
    g_pipe = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) { fprintf(stderr, "open pipe failed %lu\n", GetLastError()); return 1; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);
    fprintf(stderr, "bridge: connected\n");

    HANDLE t = CreateThread(NULL, 0, pipe_to_stdout, NULL, 0, NULL);
    unsigned char buf[65536];
    DWORD n = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        if (!ReadFile(in, buf, sizeof buf, &n, NULL) || n == 0) break;
        DWORD w = 0;
        if (!WriteFile(g_pipe, buf, n, &w, NULL)) break;
    }
    CancelIoEx(g_pipe, NULL);
    WaitForSingleObject(t, 3000);
    return 0;
}
