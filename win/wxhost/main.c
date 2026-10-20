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
#include "wxime_engine.h"
#include "wxime_event.h"
#include "wxhost_serve.h"

/* VEH 崩溃捕获（clang PE 无 SEH __try） */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    printf("[wxhost] !! AV rip=%llu fault=%llu\n",
           (unsigned long long)ep->ContextRecord->Rip,
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    fflush(stdout);
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
