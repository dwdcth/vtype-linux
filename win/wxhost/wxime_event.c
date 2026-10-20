/* wxime_event.c —— 事件 listener：候选快照（从 probes/probe3.c 移植）
 *
 * LISTENER 跑在引擎异步分发线程上（SigBatch 机制）。纪律：
 *   1. 只做同步候选枚举（get_candidates 内部 for 循环，官方姿势）；
 *   2. 绝不调用 select_candidate 等向会话线程投任务的 API——必死锁；
 *   3. 只对候选更新类事件枚举：*(b+0x10)==b（双接口自引用）且 *(b+0x08)!=0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "wxime_event.h"
#include "wxime_engine.h"

static wxime_get_candidates_fn s_get_candidates;
static wxime_candidate g_snap[WXIME_SNAP_MAX];
static volatile LONG g_snap_count;      /* 本轮枚举已存数 */
static volatile LONG g_snap_total;      /* 本轮候选总数（可超快照上限） */
static volatile LONG g_snap_ready;      /* 快照待取 */
static volatile LONG g_enum_events;     /* 候选更新事件数 */
static volatile LONG g_skipped_events;  /* 其他事件数 */
static volatile LONG g_listener_busy;   /* listener 执行中（发键前须归零） */

int wxime_event_listener_idle(void)
{
    return g_listener_busy == 0;
}

/* ---------------- 候选 TLV 解码（ci+0x20 → 拼音/词） ----------------
 * TLV：`02|tag(4B)|序号` 头 + 0x1C 起长度前缀 UTF-8 串（可能带 [01][n] 计数
 * 前缀），前两个串 = 拼音、词。窗口 tlv+0x40（实测够覆盖）。 */
static int decode_candidate(void *ci, char *py, int py_cap, char *word, int w_cap)
{
    unsigned char *tlv = *(unsigned char **)((unsigned char *)ci + WXIME_CI_TLV_OFF);
    py[0] = word[0] = 0;
    if (!tlv || tlv < (unsigned char *)0x10000 || tlv > (unsigned char *)0x7fffffffffffULL)
        return 0;
    if (IsBadReadPtr(tlv, 0x40)) return 0;
    unsigned char *p = tlv + WXIME_TLV_STR_BASE;
    int got = 0;
    for (int s = 0; s < 4 && p < tlv + 0x40; s++) {
        if (p[0] == 1 && p[1] > 0 && p[1] < 16) p += 2;   /* 串组计数前缀 */
        int L = *p++;
        if (L <= 0 || L > 30 || p + L > tlv + 0x40) break;
        if (got == 0) { if (L < py_cap) { memcpy(py, p, L); py[L] = 0; } }
        else if (got == 1) { if (L < w_cap) { memcpy(word, p, L); word[L] = 0; } }
        p += L; got++;
    }
    return got >= 2;
}

/* get_candidates 迭代回调：返回 BOOL，非零=继续（RVA 1e2d30 反汇编实证）。
 * ci 每次复用同一栈槽，必须当场解码拷走。 */
static int cand_collect(void *ci, void *user)
{
    (void)user;
    int total = InterlockedIncrement(&g_snap_total);
    int idx = total - 1;
    if (idx < WXIME_SNAP_MAX) {
        decode_candidate(ci, g_snap[idx].pinyin, sizeof g_snap[idx].pinyin,
                         g_snap[idx].word, sizeof g_snap[idx].word);
        g_snap[idx].tlv = *(void **)((unsigned char *)ci + WXIME_CI_TLV_OFF);
    }
    return 1;   /* 继续迭代 */
}

/* ---------------- 引擎事件 listener ---------------- */
static long long __fastcall wx_listener(void *a, void *b, void *c, void *d,
                                        void *e, void *f)
{
    (void)c; (void)d; (void)e; (void)f;
    InterlockedIncrement(&g_listener_busy);
    fprintf(stderr, "[wxhost] listener 入口: a=%p b=%p\n", a, b);
    fflush(stderr);
    unsigned char *ev = (unsigned char *)b;
    if (!ev || IsBadReadPtr(ev, 0x98)) { InterlockedDecrement(&g_listener_busy); return 0; }

    /* 类型门：候选更新事件（双接口自引用 + payload 非空）才可枚举 */
    int is_cand = (*(void **)(ev + WXIME_EVENT_SELFREF_OFF) == b &&
                   *(void **)(ev + WXIME_EVENT_PAYLOFF_OFF) != NULL);
    if (!is_cand) {
        InterlockedIncrement(&g_skipped_events);
        InterlockedDecrement(&g_listener_busy);
        return 0;
    }
    InterlockedIncrement(&g_enum_events);

    if (s_get_candidates) {
        InterlockedExchange(&g_snap_count, 0);
        InterlockedExchange(&g_snap_total, 0);
        /* 同步枚举（get_candidates 内部 for 循环），iterator 在事件 b+0x90 */
        s_get_candidates(ev + WXIME_ITER_OFF, (void *)cand_collect, NULL);
        LONG total = g_snap_total;
        InterlockedExchange(&g_snap_count, total < WXIME_SNAP_MAX ? total : WXIME_SNAP_MAX);
        InterlockedExchange(&g_snap_ready, 1);
        fprintf(stderr, "[wxhost] listener: 候选事件，枚举 %ld 个，ready=1\n", total);
        fflush(stderr);
    }
    InterlockedDecrement(&g_listener_busy);
    return 0;
}

void wxime_event_attach(void *session, wxime_get_candidates_fn get_candidates)
{
    s_get_candidates = get_candidates;
    wxime_add_listener_fn add =
        (wxime_add_listener_fn)(uintptr_t)GetProcAddress(
            GetModuleHandleA("libwxhld.dll"), "wxime_add_session_listener");
    add(session, (void *)wx_listener, NULL);
    wxime_log("listener 已挂");
}

int wxime_poll_candidates(wxime_candidate *out, int max)
{
    if (!InterlockedExchange(&g_snap_ready, 0)) return 0;
    int n = g_snap_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = g_snap[i];
    return g_snap_count;
}

int wxime_event_stats(int *enum_events, int *skipped_events)
{
    if (enum_events) *enum_events = g_enum_events;
    if (skipped_events) *skipped_events = g_skipped_events;
    return (int)g_snap_total;
}
