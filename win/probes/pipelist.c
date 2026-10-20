/* 枚举 wine 前缀里的命名管道 */
#include <stdio.h>
#include <windows.h>

int main(void)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("FindFirstFile failed %lu\n", GetLastError()); return 1; }
    do {
        char buf[512];
        int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, buf, sizeof buf, NULL, NULL);
        if (n > 0) printf("%s\n", buf);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}
