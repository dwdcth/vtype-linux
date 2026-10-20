/* dbg2.c —— 深挖真值：config+0xA8 资源清单 & SessionConfig IOServices
 *
 * 基于 dbg.c 的调试框架，BP_INIT 命中时：
 *   1. dump 整个 InitInfo 0x138（含指针层）
 *   2. ★ 读 config+0xA8 = {entries_ptr, count}（sub_1808166A0/sub_180816D20 实证格式），
 *      逐条 dump 24 字节条目 + 跟随条目内指针两级（找文件路径/内存缓冲）
 * BP_CREATE 命中时：dump SessionConfig 0x138 + 深挖 +0x100..0x118 四个 IOServices 指针
 * 用法：复制到引擎目录 wine 跑（cwd=引擎目录）
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#define MAXMOD 40
static unsigned long long g_base[MAXMOD]; static char g_name[MAXMOD][64]; static int g_nmod;
static void classify(unsigned long long v){
  for(int i=0;i<g_nmod;i++) if(v>=g_base[i]&&v<g_base[i]+0x4000000){printf("    %llx ∈ %s(+%llx)\n",v,g_name[i],v-g_base[i]);return;}
}

#define BP_INIT     0x1401a0fd8ULL
#define BP_CREATE   0x1401a6633ULL

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
        /* 尾随 ASCII */
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
    wchar_t cmd[] = L"wetype_server.exe";   /* cwd=引擎目录（脚本拷贝处） */
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
                printf("[bp-event] rip=%llx p=%llx code=%lx\n",
                       (unsigned long long)ctx.Rip, p, ei->ExceptionRecord.ExceptionCode);
                fflush(stdout);
                if (p == BP_INIT || p == BP_CREATE) {
                    unsigned char orig = 0xE8;
                    WriteProcessMemory(pi.hProcess, (LPVOID)p, &orig, 1, NULL);
                    const char *tag = p == BP_INIT ? "INITINFO" : "SESSIONCONFIG";
                    printf("\n===== HIT %s (rcx=%llx) ===== 已加载模块 %d 个\n", tag, ctx.Rcx, g_nmod);
                    for(int i=0;i<g_nmod;i++) printf("  mod %llx %s\n", g_base[i], g_name[i]);
                    fflush(stdout);
                    if (p == BP_INIT) for (int off = 0; off < 0x138; off += 8) {
                        int ok; unsigned long long v = rdq(pi.hProcess, ctx.Rcx + off, &ok);
                        if (ok && v > 0x10000 && v < 0x7fffffffffffULL) classify(v);
                    }

                    if (p == BP_INIT) {
                        dump_hex(pi.hProcess, ctx.Rcx, 0x138, "InitInfo");
                        /* 关键发现：config 是栈上大结构的一段。+0x58/+0xf8/+0x108/+0x120
                           指向 config 之后的兄弟对象 —— 资源服务的家。dump 整个框架。 */
                        dump_hex(pi.hProcess, ctx.Rcx, 0x400, "STACK-FRAME-LOW(0..0x400)");
                        dump_hex(pi.hProcess, ctx.Rcx + 0x400, 0x400, "STACK-FRAME-HIGH(0x400..0x800)");
                        /* +0x58 兄弟对象深挖（虚表身份） */
                        {
                            int ok; unsigned long long sib = rdq(pi.hProcess, ctx.Rcx + 0x58, &ok);
                            if (ok && sib > 0x10000) {
                                dump_hex(pi.hProcess, sib, 0x100, "SIB+58");
                                unsigned long long vt; SIZE_T g4=0;
                                if (ReadProcessMemory(pi.hProcess,(LPCVOID)sib,&vt,8,&g4) && g4==8 && vt>0x10000) {
                                    dump_hex(pi.hProcess, vt, 0x80, "SIB+58-VTABLE");
                                }
                            }
                        }
                        /* 指针层速览 */
                        for (int off = 0; off < 0x138; off += 8) {
                            int ok; unsigned long long v = rdq(pi.hProcess, ctx.Rcx + off, &ok);
                            if (ok && v > 0x10000 && v < 0x7fffffffffffULL) {
                                unsigned char probe[32]; SIZE_T g2 = 0;
                                if (ReadProcessMemory(pi.hProcess, (LPCVOID)v, probe, 32, &g2) && g2 == 32)
                                    printf("  +%.3x -> %llx: %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
                                           off, v,
                                           probe[0],probe[1],probe[2],probe[3],probe[4],probe[5],probe[6],probe[7],
                                           probe[8],probe[9],probe[10],probe[11],probe[12],probe[13],probe[14],probe[15],
                                           probe[16],probe[17],probe[18],probe[19],probe[20],probe[21],probe[22],probe[23],
                                           probe[24],probe[25],probe[26],probe[27],probe[28],probe[29],probe[30],probe[31]);
                            }
                        }
                        /* ★ +0xA8 清单深挖：{entries, count}，条目 24B */
                        int ok1, ok2;
                        unsigned long long entries = rdq(pi.hProcess, ctx.Rcx + 0xA8, &ok1);
                        unsigned long long cnt64   = rdq(pi.hProcess, ctx.Rcx + 0xB0, &ok2);
                        printf("\n[+0xA8 清单] entries=%llx count=%llu(ok=%d/%d)\n",
                               entries, cnt64, ok1, ok2);
                        int n = (int)(cnt64 & 0xffffffff);
                        if (n < 0) n = 0;
                        if (n > 64) n = 64;
                        for (int i = 0; i < n && entries; i++) {
                            unsigned long long ea = entries + (unsigned long long)i * 24;
                            printf(" entry[%d] @%llx:\n", i, ea);
                            dump_hex(pi.hProcess, ea, 24, "   raw");
                            /* 条目内指针跟随（前 3 个 qword） */
                            for (int q = 0; q < 3; q++) {
                                int ok; unsigned long long v = rdq(pi.hProcess, ea + q * 8, &ok);
                                if (ok && v > 0x10000 && v < 0x7fffffffffffULL) {
                                    char sbuf[200]; SIZE_T g3 = 0;
                                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)v, sbuf, sizeof sbuf - 1, &g3) && g3 > 0) {
                                        sbuf[g3] = 0;
                                        /* 打字符串前先看是否像 ASCII */
                                        int printable = 0;
                                        for (int k = 0; k < 8 && k < (int)g3; k++)
                                            if (sbuf[k] >= 0x20 && sbuf[k] < 0x7f) printable++;
                                        if (printable >= 6)
                                            printf("   q%d -> %llx str: \"%.80s\"\n", q, v, sbuf);
                                        else
                                            printf("   q%d -> %llx: %02x%02x%02x%02x%02x%02x%02x%02x...\n",
                                                   q, v,
                                                   (unsigned char)sbuf[0],(unsigned char)sbuf[1],(unsigned char)sbuf[2],(unsigned char)sbuf[3],
                                                   (unsigned char)sbuf[4],(unsigned char)sbuf[5],(unsigned char)sbuf[6],(unsigned char)sbuf[7]);
                                    }
                                }
                            }
                        }
                    } else {
                        /* SESSIONCONFIG：整体 + IOServices 四指针 */
                        dump_hex(pi.hProcess, ctx.Rcx, 0x138, "SessionConfig");
                        for (int off = 0x100; off <= 0x118; off += 8) {
                            int ok; unsigned long long v = rdq(pi.hProcess, ctx.Rcx + off, &ok);
                            printf("[IOSvc +%.3x] = %llx\n", off, v);
                            if (ok && v > 0x10000 && v < 0x7fffffffffffULL)
                                dump_hex(pi.hProcess, v, 64, "  iosvc_obj");
                        }
                        /* 顺带 +0xE8 回调槽与 +0xC0 字符串 */
                        int ok; unsigned long long s = rdq(pi.hProcess, ctx.Rcx + 0xC0, &ok);
                        if (ok && s > 0x10000) dump_cstr(pi.hProcess, s, "sc+0xC0 str");
                    }

                    ctx.Rip = p;
                    SetThreadContext(t, &ctx);
                    cont = DBG_CONTINUE;
                    if (p == BP_CREATE) {
                        printf("\nCREATE CAPTURED - detach\n"); fflush(stdout);
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
            if (ev.u.LoadDll.lpBaseOfDll && g_nmod < MAXMOD) {
                char nm[64]="?"; int have=0;
                if (ev.u.LoadDll.lpImageName) {
                    unsigned long long sa; SIZE_T g;
                    if (ReadProcessMemory(pi.hProcess,(LPCVOID)ev.u.LoadDll.lpImageName,&sa,8,&g)&&g==8&&sa){
                        unsigned short w[32];
                        if (ReadProcessMemory(pi.hProcess,(LPCVOID)sa,w,64,&g)&&g>=4){
                            for(int k=0;k<g/2&&k<31;k++){ if(!w[k]){have=k;break;} nm[k*2]=w[k]&0xff; nm[k*2+1]=0; }
                        }
                    }
                }
                if(!have) GetModuleFileNameExA(pi.hProcess,(HMODULE)ev.u.LoadDll.lpBaseOfDll,nm,sizeof nm-1);
                g_base[g_nmod]=(unsigned long long)ev.u.LoadDll.lpBaseOfDll;
                for(int k=0;k<63;k++) g_name[g_nmod][k]=nm[k]; g_name[g_nmod][63]=0;
                g_nmod++;
            }
        } else if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (!bp_set) {
                unsigned char orig; SIZE_T got;
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_INIT, &orig, 1, &got) && got == 1) {
                    unsigned char cc = 0xCC;
                    unsigned char probe[8]; SIZE_T g8=0;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)BP_INIT, probe, 8, &g8);
                    printf("BP_INIT 原字节: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        probe[0],probe[1],probe[2],probe[3],probe[4],probe[5],probe[6],probe[7]);
                    WriteProcessMemory(pi.hProcess, (LPVOID)BP_INIT, &cc, 1, NULL);
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)BP_CREATE, &orig, 1, &got) && got == 1)
                        WriteProcessMemory(pi.hProcess, (LPVOID)BP_CREATE, &cc, 1, NULL);
                    bp_set = 1;
                    printf("breakpoints armed\n"); fflush(stdout);
                }
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
