/* main.c —— wxhost 演示宿主：路线 C（单 exe 进程内直调 libwxhld.dll）
 *
 * 演示完整 IME 语义链：
 *   打字 "nihao"（逐键候选表打印）→ 选候选[0] "你好"（上屏）→ 活性验证
 *
 * 用法：复制到引擎目录（BuildIn/ 旁），wine wxhost.exe [引擎目录]
 * 对照探针：win/probes/probe3.c（实证用，含诊断 dump）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include "wxime_engine.h"
#include "wxime_event.h"
#include "wxhost_serve.h"

/* 模块定位：地址 → "模块名+偏移"（找不到返回 "?"） */
static void locate_addr(unsigned long long addr, char *out, int cap)
{
    HMODULE mods[256]; DWORD nmods = 0;
    out[0] = '?'; out[1] = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &nmods))
        return;
    for (DWORD i = 0; i < nmods / sizeof(HMODULE); i++) {
        MODULEINFO mi;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi))
            continue;
        unsigned long long b = (unsigned long long)mi.lpBaseOfDll;
        if (addr >= b && addr < b + mi.SizeOfImage) {
            char name[64];
            DWORD L = GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof name - 1);
            name[L] = 0;
            snprintf(out, cap, "%s+%llx", name, addr - b);
            return;
        }
    }
}

/* VEH 崩溃捕获（clang PE 无 SEH __try）。日志走 stderr（serve 下 stdout
 * 是协议通道）；定位 rip 所属模块并栈回溯定位调用链（学习路径曾把词串
 * 字节当指针传 kernel32，fault 地址即数据内容）。 */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    unsigned long long rip = (unsigned long long)ep->ContextRecord->Rip;
    char where[80];
    locate_addr(rip, where, sizeof where);
    fprintf(stderr, "[wxhost] !! AV rip=%llx (%s) fault=%llx\n",
            rip, where,
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    void *retaddrs[24];
    WORD ndepth = CaptureStackBackTrace(0, 24, retaddrs, NULL);
    for (WORD d = 0; d < ndepth; d++) {
        unsigned long long a = (unsigned long long)retaddrs[d];
        char w2[80];
        locate_addr(a, w2, sizeof w2);
        fprintf(stderr, "[wxhost]   栈[%2d] %llx (%s)\n", d, a, w2);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    /* 两种模式：
     *   wxhost.exe --serve [引擎目录]   行协议服务（fcitx5 插件后端）
     *   wxhost.exe [引擎目录]           演示：nihao→候选→选词→活性验证 */
    int serve = argc > 1 && !strcmp(argv[1], "--serve");
    const char *engine_dir = serve ? (argc > 2 ? argv[2] : ".")
                                   : (argc > 1 ? argv[1] : ".");
    /* cwd 钉到引擎目录：引擎的用户词典（Tencent\WeType\ImeDir、1/）走
     * 相对路径——不钉的话学习数据散落在调用方 cwd（fcitx5/rime 前端
     * cwd 各异 → 学习分裂）。 */
    SetCurrentDirectoryA(engine_dir);
    AddVectoredExceptionHandler(1, crash_veh);

    wxime_engine *eng = wxime_open(engine_dir, "wxhost");
    if (!eng) { wxime_log("wxime_open 失败"); return 1; }

    if (serve) return wxhost_serve(eng);

    wxime_session *sess = wxime_session_open(eng);
    if (!sess) { wxime_log("wxime_session_open 失败"); return 2; }

    /* 打字 → 逐键打印候选表（保留最近一次非空快照供选词） */
    static wxime_candidate last_cands[WXIME_SNAP_MAX];
    int last_count = 0;
    for (const char *k = "nihao"; *k; k++) {
        int pr = wxime_send_key(sess, k, 1);
        wxime_log("input '%c' -> %d", *k, pr);
        Sleep(300);
        wxime_candidate cands[64];
        int n = wxime_poll_candidates(cands, 64);
        if (n > 0) {
            last_count = n > 64 ? 64 : n;
            memcpy(last_cands, cands, (size_t)last_count * sizeof cands[0]);
            wxime_log("候选 %d 个（打印前 8）：", n);
            for (int i = 0; i < last_count && i < 8; i++)
                wxime_log("  [%d] py=\"%s\" word=\"%s\"", i,
                          cands[i].pinyin, cands[i].word);
        }
    }

    /* 选词：候选[0] 上屏（宿主契约：上屏文本 = 所选候选的词） */
    if (last_count > 0) {
        int sr = wxime_select(sess, last_cands[0].tlv, last_cands[0].word);
        wxime_log("上屏文本：\"%s\"（select -> %d）", last_cands[0].word, sr);
        Sleep(500);
    }

    /* 活性验证：选词后再打一键（Route W 同款检查） */
    int pr2 = wxime_send_key(sess, "m", 1);
    wxime_log("input 'm'（选词后）-> %d", pr2);
    Sleep(500);

    int en = 0, sk = 0;
    wxime_event_stats(&en, &sk);
    wxime_log("done（候选事件 %d，跳过事件 %d）", en, sk);
    return 0;
}
