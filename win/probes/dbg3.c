/* dbg3.c —— 抓 wetype_server 调 wxime_config_dict 的真实词典配置表
 *
 * 背景（IDA libwxhld.dll.i64 实证，2026-10-20）：
 *   create_session 的 "Resource may not ready"（session_creator.cpp:1243）根因是
 *   ResourceMgr::Impl+0x28 的 shared_ptr<Resources> 为空 —— 它由
 *   wxime_config_dict → sub_18029F2F0 → resource_holder.cpp 的构建器（sub_18029DA20
 *   "init global db"/"Config dict by file"）填充。server 在 initialize 之后、
 *   create_session 之前调它（唯一调用点 0x1401a5f1e）。
 *
 * 本探针在 0x1401a5f1e 断点，dump：
 *   rcx -> {const entry* arr; int count; ...}  （server 栈上构造）
 *   entry = 0x18 字节 {qword 0, const char* name, int id, int 0}（server 反汇编实证）
 *   并读取每条 name 字符串。
 * 用法：复制到引擎目录 wine 跑（cwd=引擎目录）；绝不能预起 wetype_server（互斥体）。
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define BP_CFG 0x1401a5f1eULL

static unsigned long long rdq(HANDLE p, unsigned long long addr, int *ok)
{
    unsigned long long v = 0; SIZE_T got = 0;
    *ok = ReadProcessMemory(p, (LPCVOID)addr, &v, 8, &got) && got == 8;
    return v;
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
                printf("[bp-event] rip=%llx p=%llx\n",
                       (unsigned long long)ctx.Rip, p);
                fflush(stdout);
                if (p == BP_CFG) {
                    unsigned char orig = 0xE8;   /* 调用点首字节（e8 rel32） */
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("\n===== HIT wxime_config_dict CALLSITE (rcx=%llx) =====\n",
                           (unsigned long long)ctx.Rcx);
                    /* 参数结构：{entries*, count(int@+8), 版本串槽@+0x10} */
                    dump_hex(pi.hProcess, ctx.Rcx, 0x30, "ARGSTRUCT");
                    int ok1, ok2;
                    unsigned long long entries = rdq(pi.hProcess, ctx.Rcx, &ok1);
                    unsigned long long cnt64   = rdq(pi.hProcess, ctx.Rcx + 8, &ok2);
                    int n = (int)(cnt64 & 0xffffffff);
                    printf("[cfg] entries=%llx count=%d(ok=%d/%d)\n", entries, n, ok1, ok2);
                    fflush(stdout);
                    if (n < 0) n = 0;
                    if (n > 64) n = 64;
                    /* 生成 C 表格方便直接抄进 probe2 */
                    printf("\n/* ---- probe2 词典表真值（dbg3 捕获）---- */\n");
                    for (int i = 0; i < n && entries; i++) {
                        unsigned long long ea = entries + (unsigned long long)i * 0x18;
                        int oka, oki;
                        unsigned long long q0 = rdq(pi.hProcess, ea, &oka);
                        unsigned long long nm = rdq(pi.hProcess, ea + 8, &oka);
                        unsigned long long idw = rdq(pi.hProcess, ea + 0x10, &oki);
                        printf("entry[%d] @%llx {q0=%llx name=%llx id=%llx}\n", i, ea, q0, nm, idw);
                        if (oka && nm > 0x10000) dump_cstr(pi.hProcess, nm, "   name");
                    }
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    printf("\nCFG CAPTURED - detach\n"); fflush(stdout);
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
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_CFG, &orig, 1, &got) && got == 1) {
                    unsigned char cc = 0xCC;
                    unsigned char probe[8]; SIZE_T g8 = 0;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)BP_CFG, probe, 8, &g8);
                    printf("BP_CFG 原字节: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        probe[0],probe[1],probe[2],probe[3],probe[4],probe[5],probe[6],probe[7]);
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_CFG, &cc, 1, NULL);
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
