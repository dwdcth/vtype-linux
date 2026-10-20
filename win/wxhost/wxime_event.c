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

/* 组词事件词表（展示兜底——无选词凭证 tlv=NULL；不覆盖候选事件快照，
 * poll 时候选快照优先） */
static wxime_candidate g_comp[WXIME_SNAP_MAX];
static volatile LONG g_comp_n;
static volatile LONG g_comp_ready;

/* 诊断：三级指针遍历找内联 CJK UTF-8 串（WXHOST_DUMP_EV=1 时组词事件触发），
 * 用于定位组词事件携带的候选表（Route W server 从此生成逐键 candidateUpdate） */
static void scan_utf8(unsigned char *s, int len, const char *path)
{
    for (int k = 0; k + 3 <= len; k++) {
        if (s[k] >= 0xE4 && s[k] <= 0xE9 &&
            (s[k+1] & 0xC0) == 0x80 && (s[k+2] & 0xC0) == 0x80) {
            char out[65]; int n = 0, q = k;
            while (q + 3 <= len && n + 3 < 64 &&
                   s[q] >= 0xE4 && s[q] <= 0xE9 &&
                   (s[q+1] & 0xC0) == 0x80 && (s[q+2] & 0xC0) == 0x80) {
                out[n++] = s[q]; out[n++] = s[q+1]; out[n++] = s[q+2];
                q += 3;
            }
            out[n] = 0;
            if (n >= 3) fprintf(stderr, "[wxhost]   文本 %s +%x: \"%s\"\n", path, k, out);
            k = q - 1;
        }
    }
}
/* 组词事件候选提取：b+0x18 → 容器 → +0x20 起指针数组 → 元素内联 SSO 词
 * （+0x00 或 +0x10 起，size/cap 在词后 0x10/0x18）。展示用（无选词凭证，
 * tlv=NULL——凭证形态待 dbg8 抓 ni-select 现场定型）。返回词数。 */
static int parse_comp_words(unsigned char *ev, wxime_candidate *out, int max)
{
    unsigned long long arr = *(unsigned long long *)(ev + 0x18);
    if (arr <= 0x10000 || arr >= 0x7fffffffffffULL || IsBadReadPtr((void *)arr, 0x120))
        return 0;
    int n = 0;
    for (int i = 0; i < 40 && n < max; i++) {
        unsigned long long ce = *(unsigned long long *)((unsigned char *)arr + 0x20 + i * 8);
        if (ce <= 0x10000 || ce >= 0x7fffffffffffULL || IsBadReadPtr((void *)ce, 0x30))
            continue;
        unsigned char *el = (unsigned char *)ce;
        /* 词 SSO：+0x00（首元素）或 +0x10（带 tagged 元数据头的元素）。
         * 判据：起始字节是 CJK UTF-8 首字节且后续 size(词后 0x10) 合理。 */
        for (int base = 0; base <= 0x10; base += 0x10) {
            unsigned char *s = el + base;
            if (s[0] >= 0xE4 && s[0] <= 0xE9 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
                unsigned long long size = *(unsigned long long *)(s + 0x10);
                unsigned long long cap  = *(unsigned long long *)(s + 0x18);
                if (size >= 3 && size <= 60 && cap >= size) {
                    int wl = (int)(size > 60 ? 60 : size);
                    if (wl < (int)sizeof out[n].word) {
                        memcpy(out[n].word, s, (size_t)wl);
                        out[n].word[wl] = 0;
                        out[n].pinyin[0] = 0;
                        out[n].tlv = NULL;
                        n++;
                    }
                    break;
                }
            }
        }
    }
    return n;
}

static void dump_event_texts(unsigned char *ev)
{
    static int do_dump = -1;
    if (do_dump < 0) {
        const char *e = getenv("WXHOST_DUMP_EV");
        do_dump = (e && *e) ? 1 : 0;
    }
    if (!do_dump) return;
    /* b+0x18 → 候选容器：+0x20 起指针数组，每元素 +0x00/+0x10 内联 SSO 词。
     * 完整扫容器前 0x200（数组元素可能十几个）。 */
    unsigned long long arr = *(unsigned long long *)(ev + 0x18);
    if (arr > 0x10000 && arr < 0x7fffffffffffULL && !IsBadReadPtr((void *)arr, 0x200)) {
        for (int i = 0; i < 40; i++) {
            unsigned long long ce = *(unsigned long long *)((unsigned char *)arr + 0x20 + i * 8);
            if (ce <= 0x10000 || ce >= 0x7fffffffffffULL || IsBadReadPtr((void *)ce, 0x20)) continue;
            char path[32]; snprintf(path, sizeof path, "容器[%d]", i);
            scan_utf8((unsigned char *)ce, 0x20, path);
            /* 元素全貌 hex（找 TLV/拼音/序号字段） */
            if (i < 3 && !IsBadReadPtr((void *)ce, 0x40)) {
                unsigned char *x = (unsigned char *)ce;
                for (int row = 0; row < 0x40; row += 16) {
                    char asc[17];
                    for (int k = 0; k < 16; k++) {
                        unsigned char ch = x[row+k];
                        asc[k] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
                    }
                    asc[16] = 0;
                    fprintf(stderr, "[wxhost]   元素[%d]+%02x: %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X |%s|\n",
                            i, row,
                            x[row],x[row+1],x[row+2],x[row+3],x[row+4],x[row+5],x[row+6],x[row+7],
                            x[row+8],x[row+9],x[row+10],x[row+11],x[row+12],x[row+13],x[row+14],x[row+15],
                            asc);
                }
                fflush(stderr);
            }
        }
    }
    for (int off = 0x08; off <= 0x88; off += 8) {
        unsigned long long p = *(unsigned long long *)(ev + off);
        if (p <= 0x10000 || p >= 0x7fffffffffffULL || IsBadReadPtr((void *)p, 0x40)) continue;
        char p1[16]; snprintf(p1, sizeof p1, "L1+%x", off);
        scan_utf8((unsigned char *)p, 0x40, p1);
        for (int off2 = 0; off2 <= 0x38; off2 += 8) {
            unsigned long long p2 = *(unsigned long long *)((unsigned char *)p + off2);
            if (p2 <= 0x10000 || p2 >= 0x7fffffffffffULL || IsBadReadPtr((void *)p2, 0x40)) continue;
            char p2s[32]; snprintf(p2s, sizeof p2s, "L1+%x->L2+%x", off, off2);
            scan_utf8((unsigned char *)p2, 0x40, p2s);
            for (int off3 = 0; off3 <= 0x38; off3 += 8) {
                unsigned long long p3 = *(unsigned long long *)((unsigned char *)p2 + off3);
                if (p3 <= 0x10000 || p3 >= 0x7fffffffffffULL || IsBadReadPtr((void *)p3, 0x40)) continue;
                char p3s[48]; snprintf(p3s, sizeof p3s, "L1+%x->L2+%x->L3+%x", off, off2, off3);
                scan_utf8((unsigned char *)p3, 0x40, p3s);
            }
        }
    }
    fflush(stderr);
}

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
        /* 组词事件：提取容器词表更新快照（逐键候选更新 + 部分选词剩余）。
         * 词无选词凭证（tlv=NULL），S 选到时 serve 会回 ERR——凭证形态
         * 待 dbg8 抓 ni-select 现场定型。 */
        {
            wxime_candidate tmp[WXIME_SNAP_MAX];
            int n = parse_comp_words(ev, tmp, WXIME_SNAP_MAX);
            if (n > 0) {
                int m = n > WXIME_SNAP_MAX ? WXIME_SNAP_MAX : n;
                for (int i = 0; i < m; i++) g_comp[i] = tmp[i];
                InterlockedExchange(&g_comp_n, m);
                InterlockedExchange(&g_comp_ready, 1);
                fprintf(stderr, "[wxhost] listener: 组词事件词表 %d 个（首=\"%s\"）\n",
                        n, g_comp[0].word);
                fflush(stderr);
            }
        }
        dump_event_texts(ev);   /* 诊断：WXHOST_DUMP_EV=1 时扫组词事件文本 */
        InterlockedDecrement(&g_listener_busy);
        return 0;
    }
    InterlockedIncrement(&g_enum_events);

    if (s_get_candidates) {
        /* ★iterator 预校验（反汇编 wxime_get_candidates 的解引用链）：
         * iter=[b+0x90] → q1=*(iter+8)；q1 非空时 q2=*(q1+0x20)、*(q2)。
         * 实测门条件（自引用+payload）不足以排除坏 iterator——重置会话的
         * 事件会带毒 iterator，枚举在 mov rdx,[rdx]（+0xC1）崩，VEH 吞掉
         * 后引擎分发线程残废（demo 时代每轮 11 次"非致命崩"同此址）。
         * 逐级验可读，坏链直接跳过枚举。 */
        unsigned char *iter = ev + WXIME_ITER_OFF;
        unsigned long long q1 = *(unsigned long long *)(iter + 8);
        int iter_ok = 1;
        if (q1) {
            if (q1 < 0x10000 || q1 > 0x7fffffffffffULL || IsBadReadPtr((void *)q1, 0x28))
                iter_ok = 0;
            else {
                unsigned long long q2 = *(unsigned long long *)((unsigned char *)q1 + 0x20);
                if (q2 && (q2 < 0x10000 || q2 > 0x7fffffffffffULL || IsBadReadPtr((void *)q2, 8)))
                    iter_ok = 0;
            }
        }
        if (!iter_ok) {
            fprintf(stderr, "[wxhost] listener: 候选事件但 iterator 毒链，跳过枚举\n");
            fflush(stderr);
            InterlockedDecrement(&g_listener_busy);
            return 0;
        }
        InterlockedExchange(&g_comp_ready, 0);   /* 候选事件优先，作废组词表 */
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
    /* 分层：候选事件快照（带选词凭证）优先；无则组词事件词表（展示用）。 */
    if (InterlockedExchange(&g_snap_ready, 0)) {
        int n = g_snap_count;
        if (n > max) n = max;
        for (int i = 0; i < n; i++) out[i] = g_snap[i];
        return g_snap_count;
    }
    if (InterlockedExchange(&g_comp_ready, 0)) {
        int n = g_comp_n;
        if (n > max) n = max;
        for (int i = 0; i < n; i++) out[i] = g_comp[i];
        return g_comp_n;
    }
    return 0;
}

int wxime_event_stats(int *enum_events, int *skipped_events)
{
    if (enum_events) *enum_events = g_enum_events;
    if (skipped_events) *skipped_events = g_skipped_events;
    return (int)g_snap_total;
}
