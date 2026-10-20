/* 运行时结构体抓取器：调试 wetype_server，在两个调用点断下并 dump 参数
 * 断点1: 0x1401a0fd8 (wxime_initialize) — rcx = InitInfo*
 * 断点2: 0x1401a6633 (wxime_create_session) — rcx = SessionConfig*
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define BP_INIT     0x1401a0fd8ULL
#define BP_CREATE   0x1401a6633ULL

static int dump_count = 0;

static void dump_region(HANDLE p, unsigned long long addr, int len, const char *tag)
{
    unsigned char buf[0x400];
    SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(p, (LPCVOID)addr, buf, len, &got) || got == 0) {
        printf("%s @%llx: <unreadable>\n", tag, addr);
        return;
    }
    printf("%s @%llx (%zu bytes):", tag, addr, got);
    for (SIZE_T i = 0; i < got; i++) {
        if (i % 32 == 0) printf("\n  +%03llx:", (unsigned long long)i);
        printf(" %02X", buf[i]);
    }
    printf("\n"); fflush(stdout);
    /* 指针槽位顺带 dump 一层（0x100 目标，供服务对象分析） */
    for (SIZE_T off = 0; off + 8 <= got; off += 8) {
        unsigned long long v;
        memcpy(&v, buf + off, 8);
        if (v > 0x10000 && v < 0x7fffffffffffULL && !(v & 7)) {
            unsigned char probe[16]; SIZE_T pg = 0;
            if (ReadProcessMemory(p, (LPCVOID)v, probe, 16, &pg) && pg == 16) {
                printf("  +%.3llx -> %llx: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                       (unsigned long long)off, v,
                       probe[0],probe[1],probe[2],probe[3],probe[4],probe[5],probe[6],probe[7],
                       probe[8],probe[9],probe[10],probe[11],probe[12],probe[13],probe[14],probe[15]);
            }
        }
    }
    fflush(stdout);
}

int main(void)
{
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"Z:\\home\\banshan\\tmp\\wetype-win\\extracted\\3.0.1.1\\wetype_server.exe";
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    printf("pid=%lu\n", pi.dwProcessId); fflush(stdout);

    int bp_set = 0, hits = 0;
    DEBUG_EVENT ev;
    for (;;) {
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *ei = &ev.u.Exception;
            if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long pc = ctx.Rip;
                if (0) {
                    const char *tag = pc == BP_INIT + 1 ? "INITINFO" : "SESSIONCONFIG";
                    printf("\n===== HIT %s =====\n", tag); fflush(stdout);
                    dump_region(pi.hProcess, ctx.Rcx, 0x138, tag);
                    /* 恢复执行：rip 回退 1，重写 int3 */
                    ctx.Rip = pc - 1;
                    SetThreadContext(t, &ctx);
                    unsigned char one = 0xCC;
                    WriteProcessMemory(pi.hProcess, (LPVOID)(pc - 1), &one, 1, NULL);
                    hits++;
                    
                } else {
                    /* DR 未用；单步可能来自别处，继续 */
                }
                CloseHandle(t);
            } else if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                unsigned long long pc = (unsigned long long)ei->ExceptionRecord.ExceptionAddress;
                if (!bp_set && pc == 0x1400a4c50 /*随便首个系统断点后*/) {
                    /* 等加载完成后下断：直接在入口断点后设置 */
                }
                cont = DBG_EXCEPTION_NOT_HANDLED; /* 让 wine 自己的 int3 过去 */
                /* 我们的目标 int3 用 SINGLE_STEP 分开处理不了，改用 BYTE 匹配 */
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long p = ctx.Rip - 1;
                if (p == BP_INIT || p == BP_CREATE) {
                    const char *tag = p == BP_INIT ? "INITINFO" : "SESSIONCONFIG";
                    unsigned char orig = 0xE8;
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL); /* 恢复原字节，一次性 */
                    printf("\n===== HIT %s =====\n", tag); fflush(stdout);
                    dump_region(pi.hProcess, ctx.Rcx, 0x138, tag);
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    cont = DBG_CONTINUE;
                    if (p == BP_CREATE) { printf("\nCREATE CAPTURED - detach\n"); fflush(stdout); CloseHandle(t); DebugActiveProcessStop(pi.dwProcessId); break; }
                    if (p == BP_INIT) { hits++; if (hits >= 1) { /* 已取，无需再断 */ } }
                }
                CloseHandle(t);
            } else {
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            break;
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT || ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            /* 进程创建/dll 加载后（libwxhld 就绪时）埋 int3 */
            if (!bp_set) {
                /* 在第一个断点位置写 0xCC，保存原字节 */
                unsigned char orig; SIZE_T got;
                static unsigned char saved[2][1];
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_INIT, &orig, 1, &got) && got == 1) {
                    saved[0][0] = orig;
                    unsigned char cc = 0xCC;
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_INIT, &cc, 1, NULL);
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_CREATE, &orig, 1, &got) && got == 1) {
                        saved[1][0] = orig;
                        WriteProcessMemory(pi.hProcess, (LPVOID)BP_CREATE, &cc, 1, NULL);
                    }
                    bp_set = 1;
                    printf("breakpoints armed (orig bytes %02X %02X)\n", saved[0][0], saved[1][0]);
                    fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
