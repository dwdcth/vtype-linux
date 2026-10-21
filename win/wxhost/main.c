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

/* VEH 崩溃捕获（clang PE 无 SEH __try）。
 * ★get_candidates 哨兵竞态指令级恢复：+0xC1 处 mov rdx,[rdx] 解引用
 * q2==-1（任务线程并发写，预校验 TOCTOU 关不死，fcitx5 高频打字实测
 * 命中）——把 Rip 改跳该函数的 xor al,al 安全出口（+0x2e9f 处 epilogue
 * 用 lea r11,[rsp+0x418] 自恢复，+0xC1 时 rsp 恰为帧基，栈平衡成立），
 * 模拟"本事件无候选"，分发线程存活。 */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    unsigned long long rip = (unsigned long long)ep->ContextRecord->Rip;
    unsigned long long fault =
        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1];
    HMODULE lib = GetModuleHandleA("libwxhld.dll");
    unsigned long long base = lib ? (unsigned long long)lib : 0;
    if (base && fault == (unsigned long long)-1 &&
        rip >= base + 0x1e2d30 && rip < base + 0x1e2ee0) {
        ep->ContextRecord->Rip = base + 0x1e2e9f;   /* 安全出口 */
        ep->ContextRecord->Rax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    char where[80];
    locate_addr(rip, where, sizeof where);
    fprintf(stderr, "[wxhost] !! AV rip=%llx (%s) fault=%llx\n",
            rip, where, fault);
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
    /* 模式（fcitx5 插件经 wine 调用不传参数，默认必须是 serve）：
     *   无参数 / --serve [目录] → 行协议服务
     *   --demo [目录] / <目录>   → 演示（nihao→选词→活性验证，兼容旧调用） */
    const char *a1 = argc > 1 ? argv[1] : NULL;
    int serve = 1;
    const char *engine_dir = NULL;   /* NULL=自定位：exe 所在目录 */
    if (a1 && !strcmp(a1, "--demo")) {
        serve = 0;
        if (argc > 2) engine_dir = argv[2];
    } else if (a1 && a1[0] != '-') {
        serve = 0;
        engine_dir = a1;
    } else if (argc > 2) {
        engine_dir = argv[2];
    }
    /* 无参 serve（fcitx5 调用方式）：引擎目录 = 本 exe 所在目录——
     * fcitx5 spawn 不传参数也不保证 cwd，缺省 "." 会落到调用方 cwd
     * （实测词典路径错、config_dict false、create_session 抛异常）。 */
    char selfdir[MAX_PATH];
    if (!engine_dir) {
        GetModuleFileNameA(NULL, selfdir, sizeof selfdir);
        char *slash = strrchr(selfdir, '\\');
        if (slash) *slash = 0;
        engine_dir = selfdir;
    }
    /* cwd 钉到引擎目录：引擎的用户词典（Tencent\WeType\ImeDir、1/）走
     * 相对路径——不钉的话学习数据散落在调用方 cwd（fcitx5/rime 前端
     * cwd 各异 → 学习分裂）。 */
    SetCurrentDirectoryA(engine_dir);
    AddVectoredExceptionHandler(1, crash_veh);

    /* 单实例锁：fcitx5 重启拉新 wxhost 时旧实例可能还活着（wine 进程
     * 退出滞后），两实例抢用户词典 LOCK → initialize 抛 C++ 异常
     * （0xe06d7363，实测崩溃源）。等待旧实例退出（最长 15s）。 */
    HANDLE single = CreateMutexA(NULL, TRUE, "Global\\wxhost_wetype_ime");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[wxhost] 已有实例在跑，等它退出...\n");
        for (int i = 0; i < 30; i++) {
            Sleep(500);
            HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, "Global\\wxhost_wetype_ime");
            if (!h) break;   /* 旧实例已退出（互斥体销毁） */
            DWORD w = WaitForSingleObject(h, 0);
            CloseHandle(h);
            if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) break;
        }
        fprintf(stderr, "[wxhost] 旧实例已让位\n");
    }

    /* serve 模式 stdout 是协议通道：日志切 stderr 必须在 wxime_open
     * 之前（open 期日志量大，fcitx5 插件按行精确匹配 READY 会被淹） */
    if (serve) wxime_log_to_stderr();
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
