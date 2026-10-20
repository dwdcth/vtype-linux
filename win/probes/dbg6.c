/* dbg6.c —— 调试 probe2.exe，抓 SessionCreator 里 user_coordinate vcall 现场
 *
 * libwxhld+0x21E12B: mov rcx,[rcx]  ← Resources 对象解引用链起点
 *   127: mov rcx,[var_3740]   ; Resources* (config_dict 建的)
 *   12b: mov rcx,[rcx]        ; ★断这里：rcx=Resources，dump 它
 *   12e: add rcx,8 / 132: mov rdx,[rcx] / 135: mov r9,[rdx+8] / 146: call r9
 *   → 返回对（a2）传给 UserCoordinateDictWrapper（+0x2098D0），空对→崩
 * 用法：复制到引擎目录 wine 跑：wine dbg6.exe（内部 spawn probe2.exe .）
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#define OFF_A 0x28BEF0ULL   /* mov rax,[rbp+0x6b8] 门条件现场 */   /* mov rcx,[rcx] 现场（rcx=Resources） */
#define OFF_B 0x21E14AULL   /* wrapper 调用前（rdx=对[0] rax=对基址） */

static unsigned long long g_wxbase;
static unsigned long long g_bpA, g_bpB;

static unsigned long long rdq(HANDLE p, unsigned long long addr, int *ok)
{
    unsigned long long v = 0; SIZE_T got = 0;
    *ok = ReadProcessMemory(p, (LPCVOID)addr, &v, 8, &got) && got == 8;
    return v;
}

static void dump_hex(HANDLE p, unsigned long long addr, int len, const char *tag)
{
    unsigned char buf[0x120]; SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(p, (LPCVOID)addr, buf, len, &got) || got == 0) {
        printf("%s @%llx: <unreadable>\n", tag, addr);
        return;
    }
    printf("%s @%llx (%zu bytes):\n", tag, addr, got);
    for (SIZE_T i = 0; i < got; i += 16) {
        printf("  +%03zx:", i);
        for (SIZE_T j = i; j < i + 16 && j < got; j++) printf(" %02X", buf[j]);
        printf("\n");
    }
    fflush(stdout);
}

int main(void)
{
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"probe3.exe .";
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    printf("pid=%lu\n", pi.dwProcessId); fflush(stdout);

    DEBUG_EVENT ev;
    for (;;) {
        if (!WaitForDebugEvent(&ev, 20000)) {
            printf("(20s 无调试事件，线程 RIP 快照:)\n");
            HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = { sizeof te };
            if (Thread32First(ts, &te)) do {
                if (te.th32OwnerProcessID == pi.dwProcessId) {
                    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                    if (th) {
                        CONTEXT c2; c2.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(th, &c2))
                            printf("  tid=%lu rip=%llx (+%llx)\n", te.th32ThreadID,
                                   (unsigned long long)c2.Rip,
                                   (unsigned long long)(c2.Rip - g_wxbase));
                        CloseHandle(th);
                    }
                }
            } while (Thread32Next(ts, &te));
            CloseHandle(ts);
            fflush(stdout);
            break;
        }
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *ei = &ev.u.Exception;
            if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long p = ctx.Rip - 1;
                if (p == g_bpA) {
                    unsigned char orig; SIZE_T got;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)p, &orig, 1, &got);
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("[implctor] rcx=%llx r8=%llx\n",
                           (unsigned long long)ctx.Rcx, (unsigned long long)ctx.R8);
                    dump_hex(pi.hProcess, ctx.R8, 0x138, "cfg拷贝+邻域");
                    fflush(stdout);
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    g_bpA = 0;
                } else if (p == g_bpB) {
                    unsigned char orig; SIZE_T got;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)p, &orig, 1, &got);
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("\n===== HIT B (+0x21E14A) =====\n");
                    printf("rax(对基址)=%llx rdx=%llx\n",
                           (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rdx);
                    dump_hex(pi.hProcess, ctx.Rax, 0x20, "pair");
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    g_bpB = 0;
                    if (!g_bpA) {
                        printf("B captured - detach（进程将继续崩溃，现场已齐）\n");
                        fflush(stdout);
                        CloseHandle(t); DebugActiveProcessStop(pi.dwProcessId); break;
                    }
                } else {
                    cont = DBG_EXCEPTION_NOT_HANDLED;
                }
                CloseHandle(t);
            } else {
                /* AV：打现场再放行（probe3 有 VEH 自己兜） */
                if (ei->ExceptionRecord.ExceptionCode == 0xC0000005) {
                    HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                    CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                    GetThreadContext(t, &ctx);
                    printf("[AV] fault=%llx rip=%llx\n",
                           (unsigned long long)ei->ExceptionRecord.ExceptionInformation[1],
                           (unsigned long long)ctx.Rip);
                    fflush(stdout);
                    CloseHandle(t);
                }
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            break;
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (!g_wxbase && ev.u.LoadDll.lpBaseOfDll) {
                char nm[MAX_PATH] = "";
                if (ev.u.LoadDll.lpImageName) {
                    unsigned long long sa; SIZE_T g;
                    if (ReadProcessMemory(pi.hProcess,(LPCVOID)ev.u.LoadDll.lpImageName,&sa,8,&g)&&g==8&&sa){
                        unsigned short w[MAX_PATH];
                        if (ReadProcessMemory(pi.hProcess,(LPCVOID)sa,w,sizeof w,&g)&&g>=4){
                            for(int k=0;k<(int)(g/2)&&k<MAX_PATH-1;k++){ if(!w[k])break; nm[k]=(char)w[k]; }
                        }
                    }
                }
                if (!nm[0])
                    GetModuleFileNameExA((HMODULE)ev.u.LoadDll.lpBaseOfDll, NULL, nm, sizeof nm-1);
                if (strstr(nm, "wxhld")) {
                    g_wxbase = (unsigned long long)ev.u.LoadDll.lpBaseOfDll;
                    g_bpA = g_wxbase + OFF_A;
                    g_bpB = g_wxbase + OFF_B;
                    unsigned char cc = 0xCC, o1, o2; SIZE_T g1, g2;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)g_bpA, &o1, 1, &g1);
                    ReadProcessMemory(pi.hProcess, (LPCVOID)g_bpB, &o2, 1, &g2);
                    WriteProcessMemory(pi.hProcess, (LPVOID)g_bpA, &cc, 1, NULL);
                    WriteProcessMemory(pi.hProcess, (LPVOID)g_bpB, &cc, 1, NULL);
                    printf("libwxhld=%llx bpA=%llx(orig %02x) bpB=%llx(orig %02x)\n",
                           g_wxbase, g_bpA, o1, g_bpB, o2);
                    fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
