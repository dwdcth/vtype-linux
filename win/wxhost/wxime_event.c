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
/* 迭代器归还（server 0x1401b3ae1 契约：消费者持有迭代器，新表到来时
 * delete 旧的）。不归还则引擎迭代器池耗尽——fccdjny 实测第 3 键起引擎
 * 不再推候选事件（只剩组词事件）。WXHOST_DEL_ITER：1=枚举后立即归还
 * ev+0x90 内联迭代器；2=归还 *(ev+0x90) 指针形态；未设=不归还（对照）。 */
typedef void (*wxime_del_iter_fn)(void *);
static wxime_del_iter_fn s_del_iter;
static int s_del_mode;
/* ★活迭代器（server 0x1401b3ae1 契约）：候选事件的 ev+0x90 迭代器是会话
 * 当前组词的**活视图**——引擎只在前 2 键发新迭代器事件，后续键组词变了
 * 也不换（fccdjny 实测第 3 键起零候选事件）。server 持有迭代器（ctx+0x210）
 * 按需重枚举、新迭代器到来时删旧。宿主照做：listener 存地址，主线程
 * wxime_event_refresh() 重枚举拿最新表。 */
static void *volatile g_live_iter;
static volatile LONG g_refresh_lock;
static volatile LONG g_iter_new;     /* 惰性枚举：listener 存了新迭代器待枚举 */
static int s_lazy_enum;
/* 组词事件=会话状态已变信号（引擎 3+ 键不发候选事件只发组词/杂事件）。
 * listener 置位，主线程看到即刷新活迭代器重枚举。 */
static volatile LONG g_state_dirty;
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

/* 联想事件词表（sequel 结果；展示用无凭证，与组词表同层兜底） */
static wxime_candidate g_seq[WXIME_SNAP_MAX];
static volatile LONG g_seq_n;
static volatile LONG g_seq_ready;

/* sequel 待响应标志：query_sequel 发起时置位；引擎日志实证联想结果走
 * CandidateUpdatedEvent（sigCandidateUpdated + malloc iterator）但事件
 * 不满足自引用门——置位期间到达的非门事件按候选事件枚举 */
static volatile LONG g_sequel_pending;
static char g_sequel_anchor[32];       /* 本轮 sequel 的上文词（锚） */

/* 清全部待取快照（S 全量重放前用：残留组词表会被误当重放结果） */
void wxime_event_clear_all(void)
{
    InterlockedExchange(&g_snap_ready, 0);
    InterlockedExchange(&g_comp_ready, 0);
    InterlockedExchange(&g_seq_ready, 0);
}

void wxime_event_expect_sequel(const char *anchor)
{
    snprintf(g_sequel_anchor, sizeof g_sequel_anchor, "%s", anchor ? anchor : "");
    InterlockedExchange(&g_sequel_pending, 1);
}

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
    if (arr <= 0x10000 || arr >= 0x7fffffffffffULL || !wx_readable((void *)arr, 0x120))
        return 0;
    int n = 0;
    for (int i = 0; i < 40 && n < max; i++) {
        unsigned long long ce = *(unsigned long long *)((unsigned char *)arr + 0x20 + i * 8);
        if (ce <= 0x10000 || ce >= 0x7fffffffffffULL || !wx_readable((void *)ce, 0x30))
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

/* 联想事件词表（get_most_likely_sequel 结果，2026-10-21 终版实证，
 * dump 标签语义三次修正后定稿）：
 *   锚  = *(*(ev+0x78)) 处的内联触发词（"你好"）
 *   词表 = *(ev+0x08) → l1；*(l1+0x10) → 词对象数组，每对象 +0 起
 *   内联 CJK 词（"好"）。锚不匹配即丢弃（防垃圾命中）。联想 iterator
 *   是引擎内部分页类型，硬枚举 AV——只解析内联词。 */
static int is_cjk3(unsigned char *p)
{
    return p[0] >= 0xE4 && p[0] <= 0xE9 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80;
}
static int parse_sequel_words(unsigned char *ev, const char *anchor,
                              wxime_candidate *out, int max)
{
    /* 锚验证（构造竞态：让出重读 400 次） */
    if (!anchor || !*anchor) return -1;
    int al = (int)strlen(anchor);
    if (al > 24) return -1;
    unsigned long long ap = *(unsigned long long *)(ev + 0x78);
    if (ap <= 0x10000 || ap >= 0x7fffffffffffULL || !wx_readable((void *)ap, (size_t)al))
        return -1;
    int anchored = 0;
    for (int tries = 0; ; tries++) {
        if (strncmp((char *)ap, anchor, (size_t)al) == 0) { anchored = 1; break; }
        if (tries >= 400) break;
        SwitchToThread();
    }
    if (!anchored) return -1;

    /* 词表：ev+8 → l1；*(l1+0x10) → 词对象，词在其 +0 起内联
     * （dump 实证"好"恰在 +0；窗口 0x40 收词，暂单串） */
    unsigned long long l1 = *(unsigned long long *)(ev + 0x08);
    if (l1 <= 0x10000 || l1 >= 0x7fffffffffffULL || !wx_readable((void *)l1, 0x18))
        return -1;
    unsigned char *w = (unsigned char *)*(unsigned long long *)
        ((unsigned char *)l1 + 0x10);
    if (!w || (unsigned long long)w <= 0x10000 ||
        (unsigned long long)w >= 0x7fffffffffffULL || !wx_readable(w, 0x40))
        return -1;
    if (!is_cjk3(w)) return 0;   /* 锚对但无词：清标志 */
    int wl = 0;
    while (wl + 3 < 60 && is_cjk3(w + wl)) wl += 3;
    if (wl < 3) return 0;
    memcpy(out[0].word, w, (size_t)wl);
    out[0].word[wl] = 0;
    out[0].pinyin[0] = 0;
    out[0].tlv = NULL;
    return 1;
}

/* 定向深链 dump（WXHOST_DUMP_EV=1 且类型门失败时）：fccdjny 实测候选
 * 词藏于 ev+0x38/0x68/0x70 → +8/+0x20 → L3 对象的深链（“风吹草动”），
 * 比组词容器（ev+0x18）深两级。hex 打印各层对象找 SSO 词/size/stride。 */
static void dump_deep_chain(unsigned char *ev)
{
    static int do_deep = -1;
    if (do_deep < 0) {
        const char *e = getenv("WXHOST_DUMP_EV");
        do_deep = (e && *e) ? 1 : 0;
    }
    if (!do_deep) return;
    static const int L1_OFFS[] = { 0x38, 0x68, 0x70 };
    for (unsigned li = 0; li < sizeof L1_OFFS / sizeof L1_OFFS[0]; li++) {
        unsigned long long l1 = *(unsigned long long *)(ev + L1_OFFS[li]);
        if (l1 <= 0x10000 || l1 >= 0x7fffffffffffULL || !wx_readable((void *)l1, 0x40))
            continue;
        fprintf(stderr, "[wxhost] 深链 L1+%x = %llx\n", L1_OFFS[li], l1);
        for (int o2 = 0; o2 <= 0x38; o2 += 8) {
            unsigned long long l2 = *(unsigned long long *)((unsigned char *)l1 + o2);
            if (l2 <= 0x10000 || l2 >= 0x7fffffffffffULL || !wx_readable((void *)l2, 0x40))
                continue;
            for (int o3 = 0; o3 <= 0x38; o3 += 8) {
                unsigned long long l3 = *(unsigned long long *)((unsigned char *)l2 + o3);
                if (l3 <= 0x10000 || l3 >= 0x7fffffffffffULL ||
                    !wx_readable((void *)l3, 0x60))
                    continue;
                unsigned char *x = (unsigned char *)l3;
                /* 词在对象内部偏移（实测 +0x11/+0x1d…），首字节不判——
                 * hex 0x40 + 文本扫描双输出 */
                char path[48];
                snprintf(path, sizeof path, "L1+%x→L2+%x→L3+%x",
                         L1_OFFS[li], o2, o3);
                scan_utf8(x, 0x60, path);
                for (int row = 0; row < 0x40; row += 16) {
                    char asc[17];
                    for (int k = 0; k < 16; k++) {
                        unsigned char ch = x[row+k];
                        asc[k] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
                    }
                    asc[16] = 0;
                    fprintf(stderr, "[wxhost]   深obj %s +%02x: %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X |%s|\n",
                            path, row,
                            x[row],x[row+1],x[row+2],x[row+3],x[row+4],x[row+5],x[row+6],x[row+7],
                            x[row+8],x[row+9],x[row+10],x[row+11],x[row+12],x[row+13],x[row+14],x[row+15],
                            asc);
                }
                fflush(stderr);
            }
        }
    }
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
    if (arr > 0x10000 && arr < 0x7fffffffffffULL && wx_readable((void *)arr, 0x200)) {
        for (int i = 0; i < 40; i++) {
            unsigned long long ce = *(unsigned long long *)((unsigned char *)arr + 0x20 + i * 8);
            if (ce <= 0x10000 || ce >= 0x7fffffffffffULL || !wx_readable((void *)ce, 0x20)) continue;
            char path[32]; snprintf(path, sizeof path, "容器[%d]", i);
            scan_utf8((unsigned char *)ce, 0x20, path);
            /* 元素全貌 hex（找 TLV/拼音/序号字段） */
            if (i < 3 && wx_readable((void *)ce, 0x40)) {
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
        if (p <= 0x10000 || p >= 0x7fffffffffffULL || !wx_readable((void *)p, 0x40)) continue;
        char p1[16]; snprintf(p1, sizeof p1, "L1+%x", off);
        scan_utf8((unsigned char *)p, 0x40, p1);
        for (int off2 = 0; off2 <= 0x38; off2 += 8) {
            unsigned long long p2 = *(unsigned long long *)((unsigned char *)p + off2);
            if (p2 <= 0x10000 || p2 >= 0x7fffffffffffULL || !wx_readable((void *)p2, 0x40)) continue;
            char p2s[32]; snprintf(p2s, sizeof p2s, "L1+%x->L2+%x", off, off2);
            scan_utf8((unsigned char *)p2, 0x40, p2s);
            for (int off3 = 0; off3 <= 0x38; off3 += 8) {
                unsigned long long p3 = *(unsigned long long *)((unsigned char *)p2 + off3);
                if (p3 <= 0x10000 || p3 >= 0x7fffffffffffULL || !wx_readable((void *)p3, 0x40)) continue;
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
    if (!wx_readable(tlv, 0x40)) return 0;
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
        /* JSON 类型（records[0].type）：completion 型不可选（实证引擎抛异常） */
        g_snap[idx].type[0] = 0;
        const char *json = *(const char **)((unsigned char *)ci + WXIME_CI_JSON_OFF);
        if (json > (const char *)0x10000 && wx_readable((void *)json, 64)) {
            const char *t = strstr(json, "\"type\":\"");
            if (t) {
                t += 9;
                int n = 0;
                while (t[n] && t[n] != '"' && n < (int)sizeof g_snap[idx].type - 1) {
                    g_snap[idx].type[n] = t[n]; n++;
                }
                g_snap[idx].type[n] = 0;
            }
        }
    }
    return 1;   /* 继续迭代 */
}

/* ---------------- 引擎事件 listener ---------------- */
static long long __fastcall wx_listener(void *a, void *b, void *c, void *d,
                                        void *e, void *f)
{
    (void)c; (void)d; (void)e; (void)f;
    InterlockedIncrement(&g_listener_busy);
    unsigned char *ev = (unsigned char *)b;
    /* 逐事件门判跟踪（fccdjny 长句实测：第 3 键起事件静默——既不过候选
     * 门也无组词词表，本行定位失败在哪一环）：r=ev 可读性、sr=自引用门、
     * py=payload 指针。 */
    {
        int r_ok = ev && wx_readable(ev, 0x98);
        unsigned long long q1 = 0, q2 = 0;
        if (r_ok) {
            unsigned char *iter = ev + WXIME_ITER_OFF;
            q1 = *(unsigned long long *)(iter + 8);
            if (q1 > 0x10000 && q1 < 0x7fffffffffffULL &&
                wx_readable((unsigned char *)q1 + 0x20, 8))
                q2 = *(unsigned long long *)((unsigned char *)q1 + 0x20);
        }
        fprintf(stderr, "[wxhost] listener ev=%p ctx=%p r=%d sr=%d py=%p q1=%llx q2=%llx\n",
                b, c, r_ok,
                r_ok ? *(void **)(ev + WXIME_EVENT_SELFREF_OFF) == b : 0,
                r_ok ? *(void **)(ev + WXIME_EVENT_PAYLOFF_OFF) : NULL, q1, q2);
        fflush(stderr);
    }
    if (!ev || !wx_readable(ev, 0x98)) { InterlockedDecrement(&g_listener_busy); return 0; }

    /* ★门判重构（fccdjny 实测定案）：候选事件的判据 = 迭代器链有效
     * （q1 canonical 可读 + q2 ≠ -1），而非旧自引用门 ev+0x10==b——后者
     * 对逐键候选事件类（py=堆指针高位）**竞态不稳定**：同型事件有时过门
     * 有时不过（过门时实测枚举 840 词全量），曾致长句第 3 键起事件全静默。
     * 旧门仅留作统计。 */
    int is_cand = (*(void **)(ev + WXIME_EVENT_SELFREF_OFF) == b &&
                   *(void **)(ev + WXIME_EVENT_PAYLOFF_OFF) != NULL);
    unsigned char *iter = ev + WXIME_ITER_OFF;
    int iter_ok_pre = 0;
    {
        /* q1 构造竞态重试（同 q2=-1/锚重试族）：SigBatch 分发与字段写入
         * 并发，fccdjny 第 4 键起 q1 读到 0/{1,1}——短暂让出重读 */
        unsigned long long q1 = 0;
        for (int tries = 0; ; tries++) {
            q1 = *(unsigned long long *)(iter + 8);
            iter_ok_pre = 1;
            if (!q1 || q1 <= 0x10000 || q1 >= 0x7fffffffffffULL ||
                (q1 < 0x1000000000ULL) ||                      /* {1,1} 型残值 */
                !wx_readable((unsigned char *)q1 + 0x20, 8)) {
                iter_ok_pre = 0;
            } else {
                unsigned long long q2 = *(unsigned long long *)((unsigned char *)q1 + 0x20);
                if (q2 == (unsigned long long)-1) iter_ok_pre = 0;
            }
            if (iter_ok_pre || tries >= 400) break;
            SwitchToThread();
        }
    }
    if (!is_cand) {
        /* 迭代器链判据（q1/q2）曾试行替代 sr 门：fccdjny 偶得第 3 表，
         * 但 nihao 流程实测枚举出 3 词垃圾表顶掉正确表、S 重放超时——
         * 回退经典 sr 门，链判据仅留诊断（上文 trace 已打印 q1/q2）。 */
        /* sequel 响应事件：iterator 是引擎内部分页类型（实测硬枚举
         * call 空表 rip=0 AV），走内联词解析（ev+8→+8→+0x10 实证） */
comp_parse:
        InterlockedIncrement(&g_skipped_events);
        InterlockedExchange(&g_state_dirty, 1);   /* 状态变：主线程该刷新了 */
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
        /* 联想事件（sequel 结果）：仅发起过 sequel 且未消费时解析。
         * 锚不匹配（-1）保留标志——杂事件常先于联想事件到达，不能让它
         * 们消费掉标志。 */
        if (g_sequel_pending) {
            wxime_candidate tmp[WXIME_SNAP_MAX];
            int n = parse_sequel_words(ev, g_sequel_anchor, tmp, WXIME_SNAP_MAX);
            if (n >= 0) {
                InterlockedExchange(&g_sequel_pending, 0);
                if (n > 0) {
                    int m = n > WXIME_SNAP_MAX ? WXIME_SNAP_MAX : n;
                    for (int i = 0; i < m; i++) g_seq[i] = tmp[i];
                    InterlockedExchange(&g_seq_n, m);
                    InterlockedExchange(&g_seq_ready, 1);
                    fprintf(stderr, "[wxhost] listener: 联想词表 %d 个（首=\"%s\"）\n",
                            n, g_seq[0].word);
                    fflush(stderr);
                }
            }
        }
        dump_event_texts(ev);   /* 诊断：WXHOST_DUMP_EV=1 时扫组词事件文本 */
        dump_deep_chain(ev);    /* 诊断：深链词对象 hex（长句候选藏身处） */
        InterlockedDecrement(&g_listener_busy);
        return 0;
    }
    InterlockedIncrement(&g_enum_events);
cand_enum:
    if (s_get_candidates) {
        /* ★iterator 预校验（反汇编 wxime_get_candidates 的解引用链）：
         * iter=[b+0x90] → q1=*(iter+8) → q2=*(q1+0x20) → *(q2)。
         * 只挡**必崩形态**：q1 非 canonical / q1+0x20 不可读（引擎第一跳
         * 无条件解引用）；q2 == -1（sequel 联想事件实测哨兵）——且 -1 存在
         * **构造竞态**（预校验读时合法、引擎解引用时已被任务线程写 -1，
         * fault=-1 同址复现）→ 毒时短暂让出重读等写完，仍毒才跳过。
         * 其余 q2 形态一律放行：引擎对非指针 q2（32 位字段拼装/堆填充）
         * 自有分型路径，拦了误丢候选（'h' 事件 125 词表曾 25-50% 误拦）。 */
        unsigned char *iter = ev + WXIME_ITER_OFF;
        int iter_ok = 0;
        for (int tries = 0; ; tries++) {
            unsigned long long q1 = *(unsigned long long *)(iter + 8);
            iter_ok = 1;
            if (q1 && (q1 < 0x10000 || q1 > 0x7fffffffffffULL ||
                       !wx_readable((unsigned char *)q1 + 0x20, 8)))
                iter_ok = 0;
            else if (q1) {
                unsigned long long q2 = *(unsigned long long *)((unsigned char *)q1 + 0x20);
                if (q2 == (unsigned long long)-1) iter_ok = 0;
            }
            if (iter_ok || tries >= 400) break;
            SwitchToThread();
        }
        if (!iter_ok) {
            fprintf(stderr, "[wxhost] listener: iterator 致命形态"
                            "（iter=%p），跳过枚举、转组词解析\n",
                    (void *)iter);
            fflush(stderr);
            /* q2=-1 的 sequel 事件被候选门误判：枚举跳过但事件本体可能
             * 携带联想词表（组词形态）——落进下方组词解析分支 */
            goto comp_parse;
        }
        InterlockedExchange(&g_comp_ready, 0);   /* 候选事件优先，作废组词表 */
        /* ★惰性枚举（WXHOST_LAZY_ENUM=1，server/ARM 同款模型）：listener 只
         * 存迭代器地址+置标志，枚举挪到主线程 wxime_poll_candidates——
         * listener 在引擎分发线程上同步枚举 850 词耗时数 ms，实测（fccdjny
         * 第 3 键起）引擎停止推候选事件；快速返回的 listener 让事件流持续。 */
        if (s_lazy_enum) {
            void *old = InterlockedExchangePointer(&g_live_iter, ev + WXIME_ITER_OFF);
            if (old && old != (void *)(ev + WXIME_ITER_OFF) && s_del_iter)
                s_del_iter(old);
            InterlockedExchange(&g_iter_new, 1);
            InterlockedExchange(&g_snap_ready, 1);
            InterlockedExchange(&g_snap_count, -1);   /* 哨兵：待主线程枚举 */
            fprintf(stderr, "[wxhost] listener: 候选事件（惰性，迭代器已存）\n");
            fflush(stderr);
            InterlockedDecrement(&g_listener_busy);
            return 0;
        }
        InterlockedExchange(&g_snap_count, 0);
        InterlockedExchange(&g_snap_total, 0);
        /* 同步枚举（get_candidates 内部 for 循环），iterator 在事件 b+0x90 */
        s_get_candidates(ev + WXIME_ITER_OFF, (void *)cand_collect, NULL);
        LONG total = g_snap_total;
        InterlockedExchange(&g_snap_count, total < WXIME_SNAP_MAX ? total : WXIME_SNAP_MAX);
        InterlockedExchange(&g_snap_ready, 1);
        fprintf(stderr, "[wxhost] listener: 候选事件，枚举 %ld 个，ready=1（del=%d）\n",
                total, s_del_mode);
        fflush(stderr);
        /* 活迭代器契约：替换时删旧（server 同款），地址长期持有供主线程
         * 重枚举。del=1/2 实验模式改为立即归还（对照臂，已证伪泄漏论）。 */
        void *old = InterlockedExchangePointer(&g_live_iter, ev + WXIME_ITER_OFF);
        if (old && old != (void *)(ev + WXIME_ITER_OFF) && s_del_iter && !s_del_mode)
            s_del_iter(old);
        else if (s_del_iter && s_del_mode == 1) s_del_iter(ev + WXIME_ITER_OFF);
        else if (s_del_iter && s_del_mode == 2) {
            void *p = *(void **)(ev + WXIME_ITER_OFF);
            if (p) s_del_iter(p);
        }
    }
    InterlockedDecrement(&g_listener_busy);
    return 0;
}

void wxime_event_attach(void *session, wxime_get_candidates_fn get_candidates)
{
    s_get_candidates = get_candidates;
    const char *dm = getenv("WXHOST_DEL_ITER");
    s_del_mode = (dm && dm[0] == '1') ? 1 : (dm && dm[0] == '2') ? 2 : 0;
    const char *lz = getenv("WXHOST_LAZY_ENUM");
    s_lazy_enum = (lz && *lz) ? 1 : 0;
    s_del_iter = (wxime_del_iter_fn)(uintptr_t)GetProcAddress(
        GetModuleHandleA("libwxhld.dll"), "wxime_delete_candidate_iterator");
    wxime_add_listener_fn add =
        (wxime_add_listener_fn)(uintptr_t)GetProcAddress(
            GetModuleHandleA("libwxhld.dll"), "wxime_add_session_listener");
    /* 实验：注册多路 listener（ARM 注释"每个 listener 获得独立迭代器"；
     * SigBatch size 3 疑似按 listener 分发——第二/三路可能收到带新迭代器
     * 的 CandidateUpdatedEvent）。第二三路只 trace 不消费。 */
    int extra = 0;
    const char *el = getenv("WXHOST_EXTRA_LISTENERS");
    if (el && *el) extra = atoi(el);
    for (int i = 0; i < extra; i++)
        add(session, (void *)wx_listener, (void *)(intptr_t)(0x100 + i));
    add(session, (void *)wx_listener, NULL);
    wxime_log("listener 已挂（+%d 路）", extra);
}

/* 主线程活迭代器重枚举：无新候选事件时（引擎 3+ 键不再推），用 listener
 * 存下的活迭代器重读会话当前候选。返回候选数；-1=无可用迭代器/忙。
 * 与 listener 互斥（同一迭代器并发枚举危险）。 */
/* 测试并清零"组词事件已到"脏标志（主线程）：真=会话状态变过，该刷新 */
int wxime_event_state_dirty(void)
{
    return InterlockedExchange(&g_state_dirty, 0);
}

int wxime_event_refresh(void)
{
    void *it = (void *)g_live_iter;
    if (!it || !s_get_candidates) return -1;
    if (!wx_readable((unsigned char *)it + 8, 8)) return -1;
    if (InterlockedCompareExchange(&g_refresh_lock, 1, 0)) return -1;
    if (g_listener_busy) { InterlockedExchange(&g_refresh_lock, 0); return -1; }
    InterlockedExchange(&g_comp_ready, 0);
    InterlockedExchange(&g_snap_count, 0);
    InterlockedExchange(&g_snap_total, 0);
    s_get_candidates(it, (void *)cand_collect, NULL);
    LONG total = g_snap_total;
    InterlockedExchange(&g_snap_count, total < WXIME_SNAP_MAX ? total : WXIME_SNAP_MAX);
    if (total > 0) InterlockedExchange(&g_snap_ready, 1);
    InterlockedExchange(&g_refresh_lock, 0);
    return (int)total;
}

int wxime_poll_sequel(wxime_candidate *out, int max)
{
    if (InterlockedExchange(&g_seq_ready, 0)) {
        int n = g_seq_n;
        if (n > max) n = max;
        for (int i = 0; i < n; i++) out[i] = g_seq[i];
        return g_seq_n;
    }
    return 0;
}

int wxime_poll_candidates(wxime_candidate *out, int max)
{
    /* 分层：候选事件快照（带选词凭证）优先；无则组词事件词表（展示用）。 */
    if (InterlockedExchange(&g_snap_ready, 0)) {
        /* 惰性枚举：listener 只存了迭代器——在此（主线程）枚举 */
        if (InterlockedExchange(&g_iter_new, 0)) {
            void *it = (void *)g_live_iter;
            if (it && s_get_candidates && wx_readable((unsigned char *)it + 8, 8)) {
                InterlockedExchange(&g_snap_count, 0);
                InterlockedExchange(&g_snap_total, 0);
                s_get_candidates(it, (void *)cand_collect, NULL);
                LONG total = g_snap_total;
                InterlockedExchange(&g_snap_count,
                                    total < WXIME_SNAP_MAX ? total : WXIME_SNAP_MAX);
                fprintf(stderr, "[wxhost] 主线程惰性枚举 -> %ld 个\n", total);
                fflush(stderr);
            } else {
                InterlockedExchange(&g_snap_count, 0);
            }
        }
        int n = g_snap_count;
        if (n > 0) {
            if (n > max) n = max;
            for (int i = 0; i < n; i++) out[i] = g_snap[i];
            return g_snap_count;
        }
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
