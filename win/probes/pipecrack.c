/* 暴力尝试管道名：CreateFileW 探测存在性 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static const char *cands[] = {
    "wetypeserver.banshan", "Server.banshan", "server.banshan",
    "IMEServer.banshan", "ime_server.banshan", "WetypeServer.banshan",
    "3.banshan", "wetype_server.banshan", "WximeServer.banshan",
    "wxime.banshan", "server", "Server", NULL
};

int main(void)
{
    for (int i = 0; cands[i]; i++) {
        char full[256];
        snprintf(full, sizeof full, "\\\\.\\pipe\\wetype.%s", cands[i]);
        wchar_t w[256];
        MultiByteToWideChar(CP_UTF8, 0, full, -1, w, 256);
        HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (h != INVALID_HANDLE_VALUE) { printf("OPENED: %s\n", full); CloseHandle(h); }
        else if (err == ERROR_PIPE_BUSY) printf("EXISTS(BUSY): %s\n", full);
        else printf("no(%lu): %s\n", err, full);
    }
    return 0;
}
