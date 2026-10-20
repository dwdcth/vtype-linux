/* 列出指定 pid 的窗口（找 wetype_server 的隐藏消息窗口） */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static DWORD g_pid;

static BOOL CALLBACK cb(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid) {
        char cls[256], title[256];
        GetClassNameA(h, cls, sizeof cls);
        GetWindowTextA(h, title, sizeof title);
        printf("hwnd=%p cls=%s title=%s visible=%lu\n", h, cls, title, (unsigned long)IsWindowVisible(h));
        fflush(stdout);
        /* 顺带列线程 */
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    g_pid = argc > 1 ? (DWORD)strtoul(argv[1], NULL, 0) : 0;
    if (!g_pid) { printf("usage: winlist <pid>\n"); return 2; }
    EnumWindows(cb, 0);
    printf("done\n");
    return 0;
}
