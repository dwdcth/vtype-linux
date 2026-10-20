/* dbg5.c —— 抓 wetype_server InitInfo+0x18 workdir 等字符串真值
 * （probe2 目前把 workdir 填成 engine\work，而 server 的真值待确认——
 *   Resources 的 user_coordinate_dict_ 疑与工作目录下的用户词典路径有关）
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#define BP_INIT 0x1401a0fd8ULL

static unsigned long long rdq(HANDLE p, unsigned long long addr, int *ok)
{
    unsigned long long v = 0; SIZE_T got = 0;
    *ok = ReadProcessMemory(p, (LPCVOID)addr, &v, 8, &got) && got == 8;
    return v;
}

static void dump_cstr(HANDLE p, unsigned long long addr, const char *tag)
{
    char buf[300]; SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(p, (LPCVOID)addr, buf, sizeof buf - 1, &got) || got == 0) {
        printf("%s @%llx: <unreadable>\n", tag, addr);
        return;
    }
    buf[got] = 0;
    printf("%s @%llx: \"%s\"\n", tag, addr, buf);
    fflush(stdout);
}

static void dump_str_at(HANDLE p, unsigned long long base, int off, const char *tag)
{
    int ok; unsigned long long v = rdq(p, base + off, &ok);
    if (ok && v > 0x10000) dump_cstr(p, v, tag);
    else printf("%s: null\n", tag);
}

static void dump_hex(HANDLE p, unsigned long long addr, int len, const char *tag)
{
    unsigned char buf[0x400]; SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(p, (LPCVOID)addr, buf, len, &got) || got == 0) {
        printf("%s @%llx: <unreadable>\n", tag, addr);
        return;
    }
    printf("%s @%llx (%zu bytes):\n", tag, addr, got);
    for (SIZE_T i = 0; i < got; i += 16) {
        printf("  +%03zx:", i);
        for (SIZE_T j = i; j < i + 16 && j < got; j++) printf(" %02X", buf[j]);
        printf("  |");
        for (SIZE_T j = i; j < i + 16 && j < got; j++)
            printf("%c", buf[j] >= 0x20 && buf[j] < 0x7f ? buf[j] : '.');
        printf("|\n");
    }
    fflush(stdout);
}

int main(void)
{
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"wetype_server.exe";
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    printf("pid=%lu\n", pi.dwProcessId); fflush(stdout);

    int bp_set = 0;
    DEBUG_EVENT ev;
    for (;;) {
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *ei = &ev.u.Exception;
            if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long p = ctx.Rip - 1;
                if (p == BP_INIT) {
                    unsigned char orig = 0xE8;
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("\n===== HIT INITINFO (rcx=%llx) =====\n",
                           (unsigned long long)ctx.Rcx);
                    dump_hex(pi.hProcess, ctx.Rcx, 0x138, "InitInfo");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x18, "II+0x18 workdir");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x40, "II+0x40 deviceid");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x68, "II+0x68");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x78, "II+0x78");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x120, "II+0x120");
                    dump_str_at(pi.hProcess, ctx.Rcx, 0x130, "II+0x130");
                    /* 解引用层：+0xF8/+0x100/+0x108/+0x120 指向的对象 */
                    for (int off = 0xF0; off <= 0x130; off += 8) {
                        int ok; unsigned long long v = rdq(pi.hProcess, ctx.Rcx + off, &ok);
                        if (ok && v > 0x10000 && v < 0x7fffffffffffULL) {
                            char tag[32]; snprintf(tag, sizeof tag, "II+%03x ->", off);
                            dump_hex(pi.hProcess, v, 0x40, tag);
                        }
                    }
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    printf("INIT CAPTURED - detach\n"); fflush(stdout);
                    CloseHandle(t); DebugActiveProcessStop(pi.dwProcessId); break;
                } else {
                    cont = DBG_EXCEPTION_NOT_HANDLED;
                }
                CloseHandle(t);
            } else {
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            break;
        } else if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (!bp_set) {
                unsigned char orig; SIZE_T got;
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_INIT, &orig, 1, &got) && got == 1) {
                    unsigned char cc = 0xCC;
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_INIT, &cc, 1, NULL);
                    bp_set = 1;
                    printf("breakpoint armed\n"); fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
