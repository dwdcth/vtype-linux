/* wxhost_serve.c —— 行协议实现（从 Route W wetype_win_host.c 移植命令集，
 * 管道 protobuf 层替换为 wxime 直调）。
 *
 * 关键契约（win/README.md §6.5 实证，Route W 血泪）：
 *   - stdout 必须二进制模式（CRLF 陷阱：插件按 line=="READY" 精确匹配）；
 *   - S 的索引基准 = B 下发的那份快照（云候选晚到不能改变索引语义）；
 *   - S 成功后若引擎剩组词（nihao 选「你」剩 hao）→ 回 CAND（剩余候选，
 *     同时成为新快照）；整段完成 → 回 OK；
 *   - C 清空组词：ESC 清不掉候选展示状态，唯一可靠 = 会话销毁重建。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include "wxhost_serve.h"
#include "wxime_event.h"

#define MAX_CAND   72
#define POLL_MS    10
#define QUIET_MS   250      /* 候选静默窗：停键后等引擎出最终候选 */
#define WAIT_CAND  2500     /* B 命令候选总超时 */
#define WAIT_LEFT 1200     /* S 后剩余组词候选的观察窗 */

/* 下发给插件的候选快照（B 的应答表 = S 的索引基准） */
static wxime_candidate g_snap[MAX_CAND];
static int g_snap_n;

static int stdin_pending(void)
{
    static HANDLE hstdin = NULL;
    if (!hstdin) hstdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (!PeekNamedPipe(hstdin, NULL, 0, NULL, &avail, NULL)) return 0;
    return avail > 0;
}

/* 轮询直到出现新候选快照；返回候选数（0=窗口内无更新）。
 * have_new 代数语义：每次 poll 到非零结果即一次新代。 */
static int wait_candidates(ULONGLONG wait_ms, int early_exit_on_pending)
{
    /* 两类表分层：候选事件表（带选词凭证 tlv，可 S）优先；组词事件词表
     * （无凭证，展示兜底——如部分选词后的剩余词）仅在窗口内无候选表时用。
     * 插件按 B 下发的列表索引 S，故展示表必须可选中。 */
    wxime_candidate cand_ans[MAX_CAND];
    int cand_n = 0, have = 0;
    wxime_candidate comp_ans[MAX_CAND];
    int comp_n = 0;
    ULONGLONG t_last = 0;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < wait_ms) {
        Sleep(POLL_MS);
        wxime_candidate c[MAX_CAND];
        int n = wxime_poll_candidates(c, MAX_CAND);
        if (n > 0) {
            int m = n > MAX_CAND ? MAX_CAND : n;
            wxime_candidate *dst = c[0].tlv ? cand_ans : comp_ans;
            int *dn = c[0].tlv ? &cand_n : &comp_n;
            *dn = 0;
            for (int i = 0; i < m; i++) {
                if (!c[i].word[0]) continue;          /* 跳过空词槽 */
                dst[(*dn)++] = c[i];
            }
            if (*dn > 0) { have = 1; t_last = GetTickCount64(); }
        }
        if (have && early_exit_on_pending && stdin_pending()) break;
        if (have && GetTickCount64() - t_last >= QUIET_MS) break;
    }
    if (cand_n > 0) {
        g_snap_n = cand_n;
        for (int i = 0; i < cand_n; i++) g_snap[i] = cand_ans[i];
        return cand_n;
    }
    if (comp_n > 0) {
        g_snap_n = comp_n;
        for (int i = 0; i < comp_n; i++) g_snap[i] = comp_ans[i];
        return comp_n;
    }
    return 0;
}

static void reply_cand(void)
{
    printf("CAND");
    for (int i = 0; i < g_snap_n; i++) printf("\t%s", g_snap[i].word);
    printf("\n");
    fflush(stdout);
}

int wxhost_serve(wxime_engine *eng)
{
    /* mingw 文本模式会把 \n 翻成 \r\n，插件精确匹配 line=="READY" 全失配 */
    _setmode(_fileno(stdout), _O_BINARY);

    wxime_session *sess = wxime_session_open(eng);
    if (!sess) { printf("ERR\n"); fflush(stdout); return 1; }
    printf("READY\n");
    fflush(stdout);

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;
        fprintf(stderr, "[wxhost] cmd=%c len=%zu\n", line[0], L);

        if (!strcmp(line, "Q")) break;
        if (!strcmp(line, "PING")) { printf("PONG\n"); fflush(stdout); continue; }
        if (!strcmp(line, "OPT spans") || !strcmp(line, "OPT tokens")) {
            printf("ERR\n"); fflush(stdout); continue;
        }
        if (!strcmp(line, "SAVE")) { printf("OK\n"); fflush(stdout); continue; }
        if (!strcmp(line, "R")) { printf("UNCHANGED\n"); fflush(stdout); continue; }
        if (line[0] == 'H' && line[1] == 'K') { printf("ERR\n"); fflush(stdout); continue; }

        if (!strcmp(line, "C")) {
            /* 清空组词：会话销毁重建（ESC 实测清不掉候选展示状态） */
            wxime_session_reset(sess);
            g_snap_n = 0;
            printf("OK\n"); fflush(stdout);
            continue;
        }

        if ((line[0] == 'B' || line[0] == 'L') && line[1] == ' ') {
            const char *keys = line + 2;
            if (!*keys) { printf("EMPTY\n"); fflush(stdout); continue; }
            int dead = 0;
            for (const char *k = keys; *k; ++k) {
                if (wxime_send_key(sess, k, 1) < 0) { dead = 1; break; }
                if (line[0] == 'L') break;
                /* 引擎事件分发线程需要喘息：键背靠背（微秒级）会让首个
                 * SigBatch 批次被下一键竞态吞掉（demo 300ms 间隔从不触发，
                 * Route W 有管道 IO 延迟天然掩盖）。20ms 实测足够。 */
                Sleep(20);
            }
            if (dead) { printf("EMPTY\n"); fflush(stdout); continue; }
            if (wait_candidates(WAIT_CAND, 1) > 0) reply_cand();
            else { printf("EMPTY\n"); fflush(stdout); }
            continue;
        }

        if (line[0] == 'S') {
            int idx = atoi(line + 1);
            if (idx < 0 || idx >= g_snap_n || !g_snap[idx].tlv) {
                fprintf(stderr, "[wxhost] S idx=%d 无凭证(快照n=%d) → ERR\n", idx, g_snap_n);
                printf("ERR\n"); fflush(stdout);
                continue;
            }
            int sr = wxime_select(sess, g_snap[idx].tlv, g_snap[idx].word);
            if (sr != 1) {
                fprintf(stderr, "[wxhost] S idx=%d select -> %d → ERR\n", idx, sr);
                printf("ERR\n"); fflush(stdout);
                continue;
            }
            /* 部分选词：emit 后引擎若剩组词会再推候选 → 回 CAND（同时成为
             * 新快照）；整段完成 → OK。契约（jinterop）。 */
            int left = wait_candidates(WAIT_LEFT, 0);
            if (left > 0) {
                fprintf(stderr, "[wxhost] S 部分选词，剩余 %d 候选（首=%s）\n",
                        left, g_snap[0].word);
                reply_cand();
            } else {
                fprintf(stderr, "[wxhost] S idx=%d 整段完成（上屏=\"%s\"）\n",
                        idx, g_snap[idx].word);
                g_snap_n = 0;
                /* ★整段完成 = 引擎会话进入 post-commit 终态（实测事件分发
                 * 停摆，之后按键零事件）——自动销毁重建，回到初始态。
                 * 对齐插件契约：OK = 插件清缓冲，引擎侧同样干净。 */
                wxime_session_reset(sess);
                printf("OK\n"); fflush(stdout);
            }
            continue;
        }

        printf("ERR\n"); fflush(stdout);
    }
    return 0;
}
