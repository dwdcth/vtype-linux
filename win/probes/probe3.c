/* probe3.c v3 —— 路线 C（单 exe 直调）第 4 站：越过 "Resource may not ready"
 *
 * 基于 probe2.c v2 基线（不动原文件），叠加 2026-10-20 深夜四项逆向成果：
 * 1. ★ wxime_config_dict(26 词典表) —— Resources 构建触发器（IDA 链实证：
 *    SessionCreator 读全局 qword_181210720(ResourceMgr)→+0x20→Impl+0x28 的
 *    shared_ptr<Resources>；它由 config_dict → resource_holder 构建器填充。
 *    initialize 只挂 ResourceMgr，不调 config_dict 必崩 "Resource may not ready"，
 *    session_creator.cpp:1243）。表真值 = dbg3 断 server 0x1401a5f1e 捕获。
 * 2. ★ workdir 真值 = "Tencent\WeType\ImeDir"（dbg5 断 0x1401a0fd8 捕获，相对路径，
 *    引擎按 cwd 解析）——用户词典 mmap 落 <cwd>\Tencent\WeType\ImeDir\user_dict。
 * 3. deviceid 真值 20 位（dbg5 捕获）。
 * 4. SessionConfig 按 dbg2 捕获的 server 312 字节真值回放。
 * 当前卡点：UserCoordinateDictWrapper 构造（+0x2098D0 崩 +0x209AB2 rcx=0）——
 *    Resources 的 user_coordinate_dict_ 成员经 vcall(+0x21E132) 取出为空对。
 *    已知伴随现象：config_dict 期 resource_holder.cpp:814 报
 *    "clean up dir file failed remove_all 拒绝访问 user_dict"（server 的 xlog 无此报错）。
 *    用 dbg6.c 动态调试该 vcall 现场。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void *(*fn_create_session_t)(void *);
typedef void (*fn_add_listener_t)(void *, void *, void *);
typedef int  (*fn_process_input_t)(void *, const char *, int, void *, int);

/* ---------------- 日志与崩溃现场 ---------------- */
static volatile LONG g_last_slot = -1;
static void slog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[probe3] "); vprintf(fmt, ap); printf("\n"); fflush(stdout);
    va_end(ap);
}

/* ---------------- 16 槽桩虚表（同 probe2 v2） ---------------- */
static long long __fastcall slot_copy(void *self, void **out)
{
    slog("slot COPY self=%p out=%p", self, out);
    out[0] = *(void **)self;
    out[1] = (void *)0x1111;
    return (long long)out;
}
static long long __fastcall slot_subscribe(void *self, void **moved)
{
    void *obj = moved[0];
    slog("slot SUBSCRIBE(引擎注册回调对象) self=%p obj=%p", self, obj);
    moved[0] = NULL;
    if (obj) {
        if (!IsBadReadPtr(obj, 16)) {
            void **vt = *(void ***)obj;
            slog("  对象vtable=%p slot0=%p slot1=%p slot2=%p", vt, vt[0], vt[1], vt[2]);
        }
        static void *g_sink; g_sink = obj;
    }
    return 0;
}
static void *g_type_desc;
static long long __fastcall slot_type(void *self, void *unused)
{
    slog("slot TYPEDESC self=%p", self);
    return (long long)g_type_desc;
}
static long long __fastcall slot_destroy(void *self, char cond)
{
    slog("slot DESTROY self=%p cond=%d (桩不释放)", self, cond);
    return 0;
}
static char g_inner[64];
static long long __fastcall slot_getinner(void *self)
{
    slog("slot GETINNER self=%p -> %p", self, g_inner);
    return (long long)g_inner;
}
static long long __fastcall slot_misc6(void *a, void *b, void *c, void *d)
{
    InterlockedExchange(&g_last_slot, 6);
    slog("slot MISC6 a=%p b=%p c=%p d=%p", a, b, c, d);
    return 0;
}
static void *g_fake_vtable[16];
static long long __fastcall inner_stub(void *a, void *b, void *c, void *d)
{
    void **vt = *(void ***)a;
    int idx = -1;
    for (int i = 0; i < 16; i++) if (vt[i] == (void *)inner_stub) { idx = i; break; }
    InterlockedExchange(&g_last_slot, 100 + idx);
    slog("INNER slot%d self=%p b=%p c=%p d=%p", idx, a, b, c, d);
    return 0;
}

/* ---------------- 回调 ---------------- */
static long long cbA(void *a, void *b, void *c, void *d)
{
    if (b && !IsBadReadPtr(b, 200)) {
        char buf[201]; int n = 0;
        for (char *p = (char *)b; *p && n < 200; p++, n++) buf[n] = *p;
        buf[n] = 0;
        if (n > 0) slog("cbA: %s", buf);
    }
    return 0;
}
static long long cbB(void *a, void *b, void *c, void *d, void *e, void *f)
{
    slog("cbB a=%p b=%p c=%p", a, b, c);
    if (b && !IsBadReadPtr(b, 32)) {
        unsigned char *x = (unsigned char *)b;
        slog("cbB b: %02X %02X %02X %02X %02X %02X %02X %02X",
             x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7]);
    }
    return 0;
}
/* 事件对象现场 dump：b/c 疑为 {事件体, 控制块} 对 */
static void dump_evobj(const char *tag, void *o, int len)
{
    if (!o || IsBadReadPtr(o, len)) { slog("  %s: <null/unreadable>", tag); return; }
    unsigned char *x = (unsigned char *)o;
    for (int row = 0; row < len; row += 16) {
        char asc[17];
        for (int k = 0; k < 16; k++) {
            unsigned char ch = x[row+k];
            asc[k] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
        }
        asc[16] = 0;
        slog("  %s+%02x: %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X |%s|",
             tag, row,
             x[row],x[row+1],x[row+2],x[row+3],x[row+4],x[row+5],x[row+6],x[row+7],
             x[row+8],x[row+9],x[row+10],x[row+11],x[row+12],x[row+13],x[row+14],x[row+15],
             asc);
    }
    for (int q = 0; q < 2; q++) {
        unsigned long long v = *(unsigned long long *)(x + q * 8);
        if (v > 0x10000 && v < 0x7fffffffffffULL && !IsBadReadPtr((void *)v, 16)) {
            char *s = (char *)v;
            int printable = 0; long z = 0;
            for (int k = 0; k < 24; k++) { if (!s[k]) { z = 1; break; } if (s[k] >= 0x20) printable++; }
            if (z && printable >= 8) slog("  %s->q%d = %llx: \"%s\"", tag, q, v, s);
        }
    }
}

/* 候选收集回调：get_candidates(iter, cb, ctx) —— server 用法 0x1401b24db */
static int g_cand_n;
static void *g_ev_b;               /* 最近事件对象（listener 存，主线程消费） */
static volatile long g_ev_new;
static volatile long g_ev_ok;      /* 事件带有效 iterator（b+0x10 自引用 + b+0x08 非空） */
/* 选词实验：cand_cb 捕获候选[0] 的 TLV 指针与词串；listener 枚举完成后
 * （同上下文，TLV 必存活）按 g_want_select 标志调 wxime_select_candidate。
 * ABI 来自 dbg8 于 server 调用点 0x1401a3ceb 的健康现场 dump：
 *   (session, rdx=&{SSO 词串, +0x20→TLV, +0x30 idlen}, 词字节长, TLV指针,
 *    int idlen, id指针, 0, &out, 0x10, desc(NULL 即可——server 传的已释放)) */
static void *g_sel_tlv;
static char g_sel_word[64];
static int g_sel_wordlen;
static volatile long g_want_select;
static volatile long g_just_selected;   /* 主线程 select 置 1，下一事件做负载扫描 */
/* 迭代协议（反汇编 0x1801e2d30 全破译）：
 *   get_candidates(iter, cb, ctx) 同步循环调 cb(ci, ctx)，
 *   cb 返回 BOOL：非零=继续、零=停；get_candidates 本身返回 1=成功 0=失败
 *   （历史上 257 是 dump 版 dump_evobj 内 slog 残留 al≠0 的巧合全量枚举）。
 *   每事件最多 5 个候选（计数器 rbx+0xa0 < 5，inc）——完整候选表要多轮。
 *   ci 每次复用同一栈槽，回调里必须当场取走内容。 */
static int cand_cb(void *ci, void *ctx2, void *c2, void *d2)
{
    /* ci 布局（实测）：
     *   +0x20 -> TLV 记录（id/序号 + [len]UTF8 串 对：拼音、词、附属段）
     *   +0x38 -> JSON 全量报告（C 字符串直读） */
    unsigned char *tlv = *(unsigned char **)((unsigned char *)ci + 0x20);
    const char *json = *(const char **)((unsigned char *)ci + 0x38);
    char py[64] = "", word[128] = "";
    if (!IsBadReadPtr(tlv, 0x40)) {
        unsigned char *p = tlv + 0x1C;
        int got = 0;
        for (int s = 0; s < 4 && p < tlv + 0x40; s++) {
            if (p[0] == 1 && p[1] > 0 && p[1] < 16) p += 2;
            int L = *p++;
            if (L <= 0 || L > 30 || p + L > tlv + 0x40) break;
            if (got == 0) { memcpy(py, p, L); py[L] = 0; }
            else if (got == 1) { memcpy(word, p, L); word[L] = 0; }
            p += L; got++;
        }
    }
    if (g_cand_n < 8) slog("候选[%d]: py=\"%s\" word=\"%s\"", g_cand_n, py, word);
    if (g_cand_n == 0 && tlv) {
        g_sel_tlv = tlv;
        int wl = (int)strlen(word);
        if (wl > 63) wl = 63;
        memcpy(g_sel_word, word, (size_t)wl);
        g_sel_word[wl] = 0;
        g_sel_wordlen = wl;
    }
    if (g_cand_n < 2 && json && !IsBadReadPtr(json, 32)) {
        char jb[400]; int n = 0;
        while (json[n] && n < 399) { jb[n] = json[n]; n++; }
        jb[n] = 0;
        slog("  json: %s", jb);
    }
    /* 选词前置侦察：前 3 个候选做全量 dump（ci 头 + TLV 全部串）——
     * CandidateWord protobuf 有 id 字符串字段（Route W 的 commit{id} 凭证），
     * id 应藏在 TLV 头部（0x00..0x1C 未读区）或附属串里。 */
    if (g_cand_n < 3 && !IsBadReadPtr(ci, 0x40)) {
        slog("  [dump] ci=%p", ci);
        dump_evobj("ci", ci, 0x40);
        if (!IsBadReadPtr(tlv, 0xC0)) {
            dump_evobj("tlv", tlv, 0xC0);
            /* 解析 0x1C 起全部 [len] 串（打到不可解为止） */
            unsigned char *q = tlv + 0x1C;
            for (int s = 0; s < 12 && q < tlv + 0xC0; s++) {
                if (q[0] == 1 && q[1] > 0 && q[1] < 16) { slog("  [dump] 串组计数=%d", q[1]); q += 2; }
                int L = *q++;
                if (L <= 0 || L > 110 || q + L > tlv + 0xC0) break;
                char sb[112]; memcpy(sb, q, L); sb[L] = 0;
                slog("  [dump] 串#%d len=%d \"%s\"", s, L, sb);
                q += L;
            }
        }
    }
    g_cand_n++;
    return 1;   /* ★反汇编实证（wxime_get_candidates 循环尾 test al,al; jne loop）：
                   回调返回 BOOL——非零=继续迭代，零=停止。此前 void/return 0
                   都在第一个候选后自杀（get_candidates 返回 1=成功标志）。 */
}

/* 在 buf 内联找 CJK UTF-8 串（E4..E9 起头 + 两个续字节），带路径打印 */
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
            if (n >= 3) slog("  文本 %s +%x: \"%s\"", path, k, out);
            k = q - 1;
        }
    }
}

static long long cbListener(void *a, void *b, void *c, void *d, void *e, void *f)
{
    slog("LISTENER a=%p b=%p c=%p d=%p", a, b, c, d);
    {
        dump_evobj("b", b, 0x90);
        dump_evobj("c", c, 0x40);
        /* server 姿势（0x1401b24d4 实证）：listener 只把事件/iterator 存进会话
         * 对象，稍后在响应构建上下文再调 get_candidates——在引擎异步分发
         * 线程里同步重入枚举，会在 libwxhld+0x1e2df1 踩 tagged 指针崩。
         * 实测门条件：只有 b+0x10 自引用（双接口标志）且 b+0x08 非空的事件
         * （CandidateUpdated）才带有效 iterator；其余事件类型 b+0x30 区全是
         * tagged/栈残值，枚举必崩。 */
        g_ev_b = b;
        g_ev_new = 1;
        g_ev_ok = (*(void **)((unsigned char *)b + 0x10) == b &&
                   *(void **)((unsigned char *)b + 0x08) != NULL) ? 1 : 0;
        slog("事件类型判定: %s", g_ev_ok ? "候选更新(可枚举)" : "其他(跳过)");
        /* select 后的事件负载扫描：三级带路径挖 UTF-8 文本（找上屏 emit 串） */
        if (g_just_selected) {
            g_just_selected = 0;
            slog(">>> select 后事件负载扫描");
            /* 定点：L1+0x38 → 对象 +0x10 内联"你好"（疑 std::string @+0x10：
             * buf@+0x10..0x1F, size@+0x20, cap@+0x28 SSO）——dump 全貌验证 */
            {
                unsigned long long p38 = *(unsigned long long *)((unsigned char *)b + 0x38);
                if (p38 > 0x10000 && p38 < 0x7fffffffffffULL && !IsBadReadPtr((void *)p38, 0x40))
                    dump_evobj("emitObj(b+38)", (void *)p38, 0x40);
            }
            unsigned char *bb = (unsigned char *)b;
            for (int off = 0x08; off <= 0x88; off += 8) {
                unsigned long long p = *(unsigned long long *)(bb + off);
                if (p <= 0x10000 || p >= 0x7fffffffffffULL || IsBadReadPtr((void *)p, 0x40)) continue;
                scan_utf8((unsigned char *)p, 0x40, "L1");
                for (int off2 = 0; off2 <= 0x38; off2 += 8) {
                    unsigned long long p2 = *(unsigned long long *)((unsigned char *)p + off2);
                    if (p2 <= 0x10000 || p2 >= 0x7fffffffffffULL || IsBadReadPtr((void *)p2, 0x40)) continue;
                    char pth[32]; snprintf(pth, sizeof pth, "L1+%x->L2", off2);
                    scan_utf8((unsigned char *)p2, 0x40, pth);
                    for (int off3 = 0; off3 <= 0x38; off3 += 8) {
                        unsigned long long p3 = *(unsigned long long *)((unsigned char *)p2 + off3);
                        if (p3 <= 0x10000 || p3 >= 0x7fffffffffffULL || IsBadReadPtr((void *)p3, 0x40)) continue;
                        char pth3[48]; snprintf(pth3, sizeof pth3, "L1+%x->L2+%x->L3", off2, off3);
                        scan_utf8((unsigned char *)p3, 0x40, pth3);
                    }
                }
            }
        }
        /* 枚举时机对比实验结论：门内事件在 listener 里同步枚举可得完整候选表
         * （dump 时代 4 个候选）；推迟 300ms 只剩顶级候选（深层 iterator 被放）。
         * 异步线程同步重入的崩 = 对非候选事件误枚举，不是重入本身。 */
        if (g_ev_ok) {
            /* 官方 3 参签名（server 0x1401b24d4：rcx=iter, rdx=cb, r8=ctx），
             * 同步迭代（函数内部 for 循环），无第 4 参。 */
            static int (*get_cands)(void *, void *, void *);
            if (!get_cands)
                get_cands = (int (*)(void *, void *, void *))(uintptr_t)
                    GetProcAddress(GetModuleHandleA("libwxhld.dll"), "wxime_get_candidates");
            if (get_cands) {
                g_cand_n = 0;
                int gr = get_cands((unsigned char *)b + 0x90, (void *)cand_cb, NULL);
                slog("get_candidates -> %d（回调 %d 次）", gr, g_cand_n);
                g_ev_new = 0;   /* 主线程兜底不再重复枚举 */
                /* 注意：select 不能在本线程调——listener 跑在引擎异步分发线程，
                 * select_candidate 会向会话线程投任务并等待 → 死锁（实测挂死）。
                 * server 也是从 IPC 线程调的。主线程流程负责选词。 */
            }
        }
    }
    return 0;
}

/* ---------------- 词典配置表（dbg3 于 0x1401a5f1e 捕获的 server 真值）----------------
 * 条目 0x18 字节 {qword0, const char* 绝对路径, int type, int 0}；
 * type 与 BuildIn/index.json 的 type 字段一致（29 个里排除 3 个 swbd* LM）。
 */
static const struct { int type; const char *file; } k_dicts[] = {
    {0x40005, "mdltrpc.bin"}, {0x10025, "tts.bin"},        {0x1000b, "ln2py.bin"},
    {0x1000d, "wsd.bin"},      {0x1002a, "prism_v2.bin"},  {0x10001, "bwcnenmac.bin"},
    {0x40001, "md.bin"},       {0x10024, "stt.bin"},       {0x1002d, "bwcnmacv8.bin"},
    {0x10002, "bccn.bin"},     {0x1001e, "wwkvc2e.bin"},   {0x20000, "lmcnmac.bin"},
    {0x1001d, "pcc.bin"},      {0x10059, "editpinyin.bin"},{0x1002c, "noun_tag_dict.bin"},
    {0x10011, "wkvc2e.pc.bin"},{0x10023, "sepd.bin"},      {0x10000, "bwcnmac.bin"},
    {0,       "cell_dict.sc.zh"}, {7,    "cell_dict.ssrc.zh"}, {0,   "cell_dict.hf.zh"},
    {0x10056, "ckvdmac.bin"},  {0x40000, "hpmac_base.bin"},{0x1000f, "slwd_pc_second_level.bin"},
    {0x10015, "nlrgpt2_nnlm.bin"}, {0x1000a, "bwcjpmac_jianpin.bin"},
};

/* COS 客户端 API 桩（server 在 initialize 后注册，0x1401a1390 传
 * {qword,qword,fn,fn}；用户词典管线疑依赖该服务在 map 中注册） */
static long long cos_cb1(void *a, void *b, void *c, void *d)
{
    slog("cos_cb1 a=%p b=%p c=%p d=%p", a, b, c, d);
    return 0;
}
static long long cos_cb2(void *a, void *b, void *c, void *d)
{
    slog("cos_cb2 a=%p b=%p c=%p d=%p", a, b, c, d);
    return 0;
}

/* ---------------- InitInfo / SessionConfig ---------------- */
static unsigned char ii[0x138];
static unsigned char sc[0x138];
static char os_str[] = "Windows.10.0.19045.0";   /* dbg5：server 真值带 .0 后缀 */
static unsigned char dev_obj8[16]  = { 6,0,0,0, 3,0,0,0, 0x03,0x00,0x46,0x81, 0,0,0,0 };
static unsigned int  obj_108[4]    = { 11, 0, 1, 0 };
static unsigned char guid_118[24]  = { 0x08,0x00,0x28,0x00, 0x07,0x00,0x44,0x80,
                                       0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
static unsigned long long obj_130[4] = { 0x5e8, 0, 0, 0 };
/* InitInfo+0x108 的 POD 真值（dbg5 解引用 dump，64 字节）：
 * {int 17, int 4}{0}{0x20000,0}{ptr@+0x20 -> 第二 POD}{0}{int 17, int 1}{0}{0} */
static unsigned long long pod2_108[8] = { 0x0000000100000011ULL, 0, 0, 0, 0, 0, 0, 0 };
static unsigned long long pod_108[8] = {
    0x0000000400000011ULL, 0,
    0x0000000000020000ULL, 0,
    (unsigned long long)pod2_108, 0,
    0x0000000100000011ULL, 0,
};

/* VEH 崩溃捕获 */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    printf("[probe3] !! AV 崩溃 rip=%llu fault=%llu last_slot=%d\n",
           (unsigned long long)ep->ContextRecord->Rip,
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
           g_last_slot);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* libwxhld wine 运行基址 → RVA（dbg6 断点换算用；也打印给人工对照） */
static void report_module_bases(void)
{
    HMODULE m = GetModuleHandleA("libwxhld.dll");
    slog("libwxhld 基址 = %p（崩溃 RVA = rip-基址，IDA 地址 = 0x180000000+RVA）", (void *)m);
}

int main(int argc, char **argv)
{
    const char *engine_dir = argc > 1 ? argv[1] : ".";

    /* 桩虚表装配 */
    g_fake_vtable[0]  = (void *)slot_copy;     g_fake_vtable[1]  = (void *)slot_copy;
    g_fake_vtable[2]  = (void *)slot_subscribe;
    g_fake_vtable[3]  = (void *)slot_type;
    g_fake_vtable[4]  = (void *)slot_destroy;
    g_fake_vtable[5]  = (void *)slot_getinner;
    g_fake_vtable[6]  = (void *)slot_misc6;
    for (int i = 7; i < 16; i++) g_fake_vtable[i] = (void *)slot_copy;
    g_fake_vtable[9]  = (void *)slot_subscribe;
    g_fake_vtable[10] = (void *)slot_type;
    g_fake_vtable[11] = (void *)slot_destroy;
    g_fake_vtable[12] = (void *)slot_getinner;
    static unsigned long long desc[8];
    memset(desc, 0, sizeof desc);
    desc[0] = 0xDEAD0001;
    g_type_desc = desc;
    static void *inner_vt[16];
    for (int i = 0; i < 16; i++) inner_vt[i] = (void *)inner_stub;
    *(void ***)g_inner = inner_vt;
    static void *fake_obj[4];
    fake_obj[0] = (void *)g_fake_vtable;

    /* InitInfo —— dbg5 真值 */
    memset(ii, 0, sizeof ii);
    *(void **)(ii + 0x00) = (void *)cbA;
    *(char **)(ii + 0x18) = "Tencent\\WeType\\ImeDir";   /* ★ workdir 真值（相对路径） */
    *(char **)(ii + 0x40) = "13451671603782742029";      /* ★ 20 位 deviceid 真值 */
    *(int *)(ii + 0x48) = 20;
    *(int *)(ii + 0x4c) = 2;
    *(int *)(ii + 0x50) = 6;
    *(char **)(ii + 0x68) = os_str;
    *(char **)(ii + 0x78) = os_str;
    *(int *)(ii + 0x80) = (int)sizeof os_str - 1;
    /* fnB 实验位：置 NULL（SigBatch 时 loguru 按 id 找回调失败疑与此 sink 有关）*/
    *(void **)(ii + 0x88) = NULL;
    *(int *)(ii + 0xc0) = 3;
    *(void **)(ii + 0xd0) = fake_obj;
    *(void **)(ii + 0xd8) = dev_obj8;
    *(int *)(ii + 0xf0) = 0xA0;
    *(void **)(ii + 0x108) = obj_108;
    *(void **)(ii + 0x118) = guid_118;
    *(void **)(ii + 0x130) = obj_130;
    /* ★ 用户词典装配门（dbg4 于 libwxhld+0x29EB8B 实证）：门对象 = InitInfo+0xF8
     * 处的字段，gate = *(InitInfo+0x108) != 0。server 真值（dbg5）：
     *   +0xF8  = -1
     *   +0x100 = 0x7fff40000062（标记指针，照抄）
     *   +0x108 -> POD {17,4,1,0, 0x20000,0, &pod2,0, 17,1,0,0}
     * probe3 此前全 0 → config_dict 跳过 0x86 包装器构造 → create_session 崩。 */
    *(unsigned long long *)(ii + 0xF8) = (unsigned long long)-1;
    *(unsigned long long *)(ii + 0x100) = 0x7fff40000062ULL;
    *(void **)(ii + 0x108) = pod_108;

    /* SessionConfig —— dbg2 于 server 0x1401a6633 捕获的 312 字节真值回放
     * （+0xC0 客户端名指针自定；+0xE8 server 回调先 NULL） */
    static const unsigned char sc_real[0x138] = {
        /* +000 */ 0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x01,0x01,0x01,0x01,0x00,0x00,0x00,
        /* +010 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +020 */ 0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00, 0x01,0x94,0x01,0x40,0x00,0x01,0x01,0x00,
        /* +030 */ 0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x01, 0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        /* +040 */ 0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00, 0,0,0,0,0,0,0,0,
        /* +050 */ 0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00, 0,0,0,0,0,0,0,0,
        /* +060 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +070 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +080 */ 0,0,0,0,0,0,0,0, 0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,
        /* +090 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +0A0 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +0B0 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +0C0 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +0D0 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0x0C,0,0,0,
        /* +0E0 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +0F0 */ 0,0,0,0,0,0,0,0, 0x01,0,0,0,0,0,0,0,
        /* +100 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +110 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* +120 */ 0,0,0,0,0,0x01,0x00,0x01, 0x01,0,0,0,0,0,0,0,
        /* +130 */ 0,0,0,0,0,0,0,0,
    };
    memcpy(sc, sc_real, sizeof sc_real);
    *(const char **)(sc + 0xC0) = "probe3";
    *(int *)(sc + 0xDC) = 5;

    AddVectoredExceptionHandler(1, crash_veh);
    HMODULE h = LoadLibraryA("libwxhld.dll");
    if (!h) { slog("LoadLibrary 失败 %lu", GetLastError()); return 1; }
    slog("dll loaded");
    report_module_bases();

    int (*initialize)(void *) = (int (*)(void *))GetProcAddress(h, "wxime_initialize");
    fn_create_session_t create_session = (fn_create_session_t)GetProcAddress(h, "wxime_create_session");
    fn_add_listener_t add_listener = (fn_add_listener_t)GetProcAddress(h, "wxime_add_session_listener");
    fn_process_input_t process_input = (fn_process_input_t)GetProcAddress(h, "wxime_process_input");
    int (*warm_up)(void *) = (int (*)(void *))GetProcAddress(h, "wxime_warm_up_session");

    slog("initialize...");
    int r = initialize(ii);
    slog("wxime_initialize -> %d", r);
    if (r != 0) return 2;

    /* ★ server 在 initialize 后、config_dict 前调 wxime_register_client_cos_api
     * （0x1401a1390，参数 = {qword,qword,fn,fn}）。probe3 此前跳过它——
     * 用户词典装配门疑依赖 COS 服务注册。桩回调先探路。 */
    {
        static unsigned long long cosapi[4];
        memset(cosapi, 0, sizeof cosapi);
        cosapi[2] = (unsigned long long)cos_cb1;
        cosapi[3] = (unsigned long long)cos_cb2;
        int (*reg_cos)(void *) =
            (int (*)(void *))GetProcAddress(h, "wxime_register_client_cos_api");
        if (reg_cos) slog("register_client_cos_api -> %d", reg_cos(cosapi));
        else slog("register_client_cos_api 不存在");
    }
    /* 检查 initialize 是否把门字段搬走了（move 语义会清零源） */
    slog("ii 门字段回读: +F8=%llx +100=%llx +108=%llx",
         *(unsigned long long *)(ii + 0xF8),
         *(unsigned long long *)(ii + 0x100),
         *(unsigned long long *)(ii + 0x108));

    /* ★ 词典配置：触发 Resources 构建（server 在 initialize 后、create_session 前调用） */
    {
        char (*paths)[300] = calloc(sizeof k_dicts / sizeof k_dicts[0], sizeof *paths);
        unsigned char *entries = calloc(sizeof k_dicts / sizeof k_dicts[0], 0x18);
        char base[MAX_PATH];
        if (!GetFullPathNameA(engine_dir, sizeof base, base, NULL))
            snprintf(base, sizeof base, "%s", engine_dir);
        int n = (int)(sizeof k_dicts / sizeof k_dicts[0]);
        for (int i = 0; i < n; i++) {
            snprintf(paths[i], 300, "%s\\BuildIn\\%s", base, k_dicts[i].file);
            unsigned char *e = entries + (size_t)i * 0x18;
            *(unsigned long long *)(e + 0x00) = 0;
            *(const char **)(e + 0x08) = paths[i];
            *(int *)(e + 0x10) = k_dicts[i].type;
            *(int *)(e + 0x14) = 0;
        }
        unsigned char cfg[0x30];
        memset(cfg, 0, sizeof cfg);
        *(void **)(cfg + 0x00) = entries;
        *(int *)(cfg + 0x08) = n;
        /* ★ 门条件（objdump 实证）：构建器 sub_18029DA20 把 arg2 存在
         * [entry_rsp+0x10]（=rbp+0x6b8），门 = *(arg2+0x10) != 0 且会被当
         * MSVC 长形式 std::string 的堆指针解引用（SSO '1' 实测 fault=0x31）。
         * server 传 {堆指针, size=0, cap}。放合法长形式串：malloc 缓冲 +
         * size/cap（版本串短，operator= 复用缓冲不 free）。 */
        static char *versbuf;
        if (!versbuf) { versbuf = (char *)malloc(64); memset(versbuf, 0, 64); versbuf[0] = '1'; }
        *(char **)(cfg + 0x10) = versbuf;
        *(unsigned long long *)(cfg + 0x20) = 1;    /* size */
        *(unsigned long long *)(cfg + 0x28) = 63;   /* cap（长形式）*/
        int (*config_dict)(void *) = (int (*)(void *))GetProcAddress(h, "wxime_config_dict");
        slog("config_dict(条目 %d)...", n);
        int cr = config_dict(cfg);
        slog("wxime_config_dict -> %d", cr);
        /* 用户词典 mmap 异步构建（ImeDir/user_dict/*），server 从 init 到首会话
         * 有 ~20s 余量；直调等待，否则 Resources 的 user_coordinate_dict_ 为空 */
        slog("等待用户词典异步构建(5s)...");
        Sleep(5000);
    }

    slog("create_session...");
    void *sess = create_session(sc);
    slog("session = %p", sess);
    if (!sess) return 3;

    add_listener(sess, (void *)cbListener, NULL);
    slog("listener 已挂");
    if (warm_up) { slog("warm_up..."); warm_up(sess); }
    /* r9 = 堆上 context/out 结构（dbg7 于 0x1401af05d 实证 server 传 malloc 出的
     * 缓冲，内含客户端名等；传 NULL 会穿空指针崩 vmovdqu）。第 5 参恒 0x10。 */
    static unsigned char *inctx;
    if (!inctx) { inctx = (unsigned char *)calloc(1, 0x100); }
    for (const char *k = "nihao"; *k; k++) {
        slog("input '%c'", *k);
        memset(inctx, 0, 0x100);
        int pr = process_input(sess, k, 1, inctx, 0x10);
        slog("  -> %d", pr);
        Sleep(300);
        /* 候选枚举已在 listener 内同步完成（门内事件），主线程无需再做。 */
    }
    /* ★主线程选词实验（Route W 同款流程：打完 nihao → 选候选[0] 你好）。
     * TLV 是 listener 枚举时捕获的堆指针；'a'/'o' 不产生候选事件，理论上
     * 最近一次候选 TLV（h 事件）仍存活。 */
    if (g_sel_tlv) {
        static unsigned char sst[0x40];
        static unsigned char idbuf[128];
        memset(sst, 0, sizeof sst);
        memset(idbuf, 0, sizeof idbuf);
        memcpy(sst, g_sel_word, (size_t)g_sel_wordlen);
        *(unsigned long long *)(sst + 0x10) = g_sel_wordlen;  /* string size */
        *(unsigned long long *)(sst + 0x18) = 0xF;            /* SSO cap */
        *(void **)(sst + 0x20) = g_sel_tlv;
        *(unsigned long long *)(sst + 0x30) = 100;
        *(unsigned long long *)(sst + 0x38) = 0x6F;
        long long outv[4]; memset(outv, 0, sizeof outv);
        int (*sel_cand)(void *, void *, int, void *, int, void *, int, void *, int, void *) =
            (int (*)(void *, void *, int, void *, int, void *, int, void *, int, void *))(uintptr_t)
            GetProcAddress(GetModuleHandleA("libwxhld.dll"), "wxime_select_candidate");
        g_just_selected = 1;
        slog("select_candidate(word=\"%s\" len=%d) ...", g_sel_word, g_sel_wordlen);
        int sr = sel_cand(sess, sst, g_sel_wordlen, g_sel_tlv, 100, idbuf, 0, outv, 0x10, NULL);
        slog("select_candidate -> %d out={%llx,%llx}", sr,
             (unsigned long long)outv[0], (unsigned long long)outv[1]);
        Sleep(500);
        /* 选词后再打一键验证会话活性（Route W 的 aliveness 检查） */
        slog("input 'm'（选词后活性验证）");
        memset(inctx, 0, 0x100);
        int pr2 = process_input(sess, "m", 1, inctx, 0x10);
        slog("  -> %d", pr2);
        Sleep(500);
    }
    Sleep(2000);
    slog("done");
    return 0;
}
