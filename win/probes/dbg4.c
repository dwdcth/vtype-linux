/* dbg4.c —— server 侧通用断点捕获（libwxhld 相对地址）
 *
 * 当前用途：断 Impl::Impl 入口（libwxhld+0x28BEF0），dump 三参 + r8 指向的
 * 208 字节 wxime_config 拷贝及其栈邻域（读 0x138）——对比 probe3 侧（dbg6 同址），
 * 找 config_dict 用户词典装配门（+0x29EB8B 读 [obj+0x10]）在 server 侧非零、
 * probe3 侧为零的活指针来源。
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#define OFF_BP 0x28BEF0ULL

static unsigned long long g_wxbase;
static unsigned long long g_bpat;

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
        printf("\n");
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
                        DEBUG_ONLY_THIS_PROCESS, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    printf("pid=%lu\n", pi.dwProcessId); fflush(stdout);

    int hits = 0;
    DEBUG_EVENT ev;
    for (;;) {
        if (!WaitForDebugEvent(&ev, 20000)) break;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *ei = &ev.u.Exception;
            if (ei->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId);
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(t, &ctx);
                unsigned long long p = ctx.Rip - 1;
                if (g_bpat && p == g_bpat) {
                    unsigned char orig; SIZE_T got;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)p, &orig, 1, &got);
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    printf("[implctor %d] rcx=%llx rdx=%llx r8=%llx\n",
                           ++hits, (unsigned long long)ctx.Rcx,
                           (unsigned long long)ctx.Rdx, (unsigned long long)ctx.R8);
                    dump_hex(pi.hProcess, ctx.R8, 0x138, "cfg拷贝+邻域");
                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    if (hits >= 3) {
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
                    g_bpat = g_wxbase + OFF_BP;
                    unsigned char cc = 0xCC, orig; SIZE_T got;
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)g_bpat, &orig, 1, &got) && got == 1)
                        WriteProcessMemory(pi.hProcess, (LPVOID)g_bpat, &cc, 1, NULL);
                    printf("libwxhld=%llx bp=%llx armed\n", g_wxbase, g_bpat);
                    fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
