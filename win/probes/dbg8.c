/* dbg8.c —— 断 server 调 wxime_select_candidate 的现场（绝对地址 0x1401a3ceb）
 *
 * 目的：抓 C ABI 的真实 10 参 + 0x20 字节候选描述符 + id 字符串内容，
 * 供 probe3 直调复刻。配套驱动：select_test.exe mode1（管道 commit{id}）。
 * 用法：复制到引擎目录，wine dbg8.exe（内部 spawn wetype_server.exe），
 * 另一终端 wine select_test.exe 1。
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#define BP_SELECT 0x1401a3cebULL   /* call wxime_select_candidate */
#define BP_SELECT_DEF 0x1401a4001ULL /* call wxime_select_default_candidate */

static int g_armed, g_hit;
static unsigned char g_orig_sel, g_orig_seld;

static void dump_hex(HANDLE p, unsigned long long addr, int len, const char *tag)
{
    unsigned char buf[0x140]; SIZE_T got = 0;
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

static void dump_cstr(HANDLE p, unsigned long long addr, int maxlen, const char *tag)
{
    char buf[300]; SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(p, (LPCVOID)addr, buf, maxlen, &got) || got == 0) {
        printf("%s @%llx: <unreadable>\n", tag, addr);
        return;
    }
    buf[got] = 0;
    printf("%s @%llx (%zuB): \"%s\"\n", tag, addr, got, buf);
    /* 也按 hex 打（id 可能是二进制 blob 不是文本） */
    printf("%s hex:", tag);
    for (SIZE_T i = 0; i < got && i < 120; i++) printf(" %02X", (unsigned char)buf[i]);
    printf("\n");
    fflush(stdout);
}

int main(void)
{
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"wetype_server.exe logtostderr";
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    printf("pid=%lu\n", pi.dwProcessId); fflush(stdout);

    DEBUG_EVENT ev;
    for (;;) {
        if (!WaitForDebugEvent(&ev, 60000)) break;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *ei = &ev.u.Exception;
            if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long p = ctx.Rip - 1;
                if (p == BP_SELECT || p == BP_SELECT_DEF) {
                    /* 用 arm 时保存的原字节恢复（命中处读到的必是自己的 CC） */
                    unsigned char orig = (p == BP_SELECT) ? g_orig_sel : g_orig_seld;
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("\n===== HIT %s =====\n", p == BP_SELECT ? "SELECT" : "SELECT_DEF");
                    printf("rcx=%llx rdx=%llx r8=%llx r9=%llx\n",
                           (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx,
                           (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);
                    printf("rbx=%llx rsi=%llx rdi=%llx rbp=%llx\n",
                           (unsigned long long)ctx.Rbx, (unsigned long long)ctx.Rsi,
                           (unsigned long long)ctx.Rdi, (unsigned long long)ctx.Rbp);
                    /* 调用点栈参数 [rsp+0x20..0x48] */
                    unsigned long long sa[8]; SIZE_T sg;
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)(ctx.Rsp + 0x20), sa, sizeof sa, &sg) && sg == sizeof sa) {
                        printf("栈参: ");
                        for (int i = 0; i < 6; i++) printf("[+0x%x]=%016llx ", 0x20 + i * 8, sa[i]);
                        printf("\n");
                        /* [+0x28] = id 串指针（select_test 实证 idlen=100 在 [+0x20] 低位） */
                        dump_cstr(pi.hProcess, sa[1], 130, "id串([+0x28])");
                    }
                    dump_hex(pi.hProcess, ctx.Rdx, 0x40, "ctx(rdx)");
                    dump_hex(pi.hProcess, ctx.R9, 0x40, "r9对象");
                    /* 描述符数组（select: rsi 指向 0x20 字节条目） */
                    dump_hex(pi.hProcess, ctx.Rsi, 0x40, "desc数组(rsi)");
                    /* 描述符里的两个字符串 */
                    unsigned long long d0, d1;
                    if (ctx.Rsi && ReadProcessMemory(pi.hProcess, (LPCVOID)ctx.Rsi, &d0, 8, &sg) && sg == 8) {
                        ReadProcessMemory(pi.hProcess, (LPCVOID)(ctx.Rsi + 0x10), &d1, 8, &sg);
                        dump_cstr(pi.hProcess, d0, 130, "desc[0].str1");
                        if (d1) dump_cstr(pi.hProcess, d1, 130, "desc[0].str2");
                    }
                    /* rdi = server 会话候选状态对象 */
                    dump_hex(pi.hProcess, ctx.Rdi, 0x60, "状态对象(rdi)");
                    /* select_def 的 rdx 数组 */
                    if (p == BP_SELECT_DEF && ctx.Rdx) dump_hex(pi.hProcess, ctx.Rdx, 0x40, "sd数组(rdx)");
                    fflush(stdout);
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    g_hit++;
                    if (g_hit >= 4) {
                        CloseHandle(t); DebugActiveProcessStop(pi.dwProcessId); break;
                    }
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
            if (!g_armed) {
                SIZE_T got;
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_SELECT, &g_orig_sel, 1, &got) && got == 1) {
                    ReadProcessMemory(pi.hProcess, (LPCVOID)BP_SELECT_DEF, &g_orig_seld, 1, &got);
                    unsigned char cc = 0xCC;
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_SELECT, &cc, 1, NULL);
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_SELECT_DEF, &cc, 1, NULL);
                    g_armed = 1;
                    printf("breakpoints armed (orig %02x %02x)\n", g_orig_sel, g_orig_seld); fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
