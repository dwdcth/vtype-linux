/* wxime_engine.c —— 引擎生命周期实现（从 probes/probe3.c 实证逻辑移植）
 *
 * 初始化序列（顺序敏感，全部为 2026-10 动态实证，见 win/README.md §6.0）：
 *   LoadLibrary(libwxhld.dll)
 *   → wxime_initialize(InitInfo 真值)            必须返回 0
 *   → wxime_register_client_cos_api(桩)          返回 -1 也可继续
 *   → wxime_config_dict(26 词典表 + 版本串门)     触发 Resources 构建
 *   → 等待 5s                                    用户词典 mmap 异步落盘
 *   → wxime_create_session(SessionConfig 312B)   返回会话句柄
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "wxime_engine.h"
#include "wxime_event.h"
#include "wxime_event.h"

/* ---------------- 日志 ----------------
 * serve 模式 stdout 是协议通道，所有日志必须走 stderr（由 wxhost_serve
 * 置位 wxime_log_stderr_only）。 */
static int g_log_stderr_only;

void wxime_log_to_stderr(void) { g_log_stderr_only = 1; }

void wxime_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(g_log_stderr_only ? stderr : stdout, "[wxhost] ");
    vfprintf(g_log_stderr_only ? stderr : stdout, fmt, ap);
    fprintf(g_log_stderr_only ? stderr : stdout, "\n");
    fflush(g_log_stderr_only ? stderr : stdout);
    va_end(ap);
}

/* ---------------- 16 槽桩虚表（InitInfo+0xD0 双接口侵入件） ----------------
 * 语义已破译（win/README.md §6.0.3）：slot0/1、7/8=copy，2/9=subscribe，
 * 3/10=类型描述符，4/11=条件析构，5/12=inner，6=数据不会被调。 */
static volatile LONG g_last_slot = -1;

static long long __fastcall slot_copy(void *self, void **out)
{
    wxime_log("slot COPY self=%p out=%p", self, out);
    out[0] = *(void **)self;
    out[1] = (void *)0x1111;
    return (long long)out;
}
static long long __fastcall slot_subscribe(void *self, void **moved)
{
    void *obj = moved[0];
    wxime_log("slot SUBSCRIBE(引擎注册回调对象) self=%p obj=%p", self, obj);
    moved[0] = NULL;
    return 0;
}
static unsigned long long s_type_desc[8];
static long long __fastcall slot_type(void *self, void *unused)
{
    wxime_log("slot TYPEDESC self=%p", self);
    return (long long)s_type_desc;
}
static long long __fastcall slot_destroy(void *self, char cond)
{
    wxime_log("slot DESTROY self=%p cond=%d (桩不释放)", self, cond);
    return 0;
}
static char s_inner[64];
static long long __fastcall slot_getinner(void *self)
{
    wxime_log("slot GETINNER self=%p", self);
    return (long long)s_inner;
}
static long long __fastcall slot_misc6(void *a, void *b, void *c, void *d)
{
    InterlockedExchange(&g_last_slot, 6);
    wxime_log("slot MISC6 a=%p b=%p c=%p d=%p", a, b, c, d);
    return 0;
}
static void *s_fake_vtable[16];
static void *s_inner_vtable[16];
static void *s_fake_obj[4];

/* 无副作用可读检查：见 wxime_engine.h 注释（wine IsBadReadPtr 自身 AV 教训） */
int wx_readable(const void *p, size_t n)
{
    if (!p) return 0;
    unsigned long long a = (unsigned long long)p;
    unsigned long long end = a + (unsigned long long)n;
    if (end < a) return 0;               /* 地址回绕 */
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || prot == 0 || (mbi.Protect & PAGE_GUARD))
            return 0;
        unsigned long long region_end =
            (unsigned long long)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= a) return 0;   /* 防御：避免死循环 */
        a = region_end;
    }
    return 1;
}

/* ---------------- 引擎日志回调（InitInfo+0x00，loguru sink） ---------------- */
static long long cbA(void *a, void *b, void *c, void *d)
{
    if (b && wx_readable(b, 200)) {
        char buf[201]; int n = 0;
        for (char *p = (char *)b; *p && n < 200; p++, n++) buf[n] = *p;
        buf[n] = 0;
        if (n > 0) fprintf(stderr, "[engine] %s\n", buf), fflush(stderr);
    }
    return 0;
}

/* ---------------- COS 客户端 API 桩（返回 -1 不影响主流程） ---------------- */
static long long cos_cb1(void *a, void *b, void *c, void *d)
{
    wxime_log("cos_cb1 a=%p b=%p", a, b);
    return 0;
}
static long long cos_cb2(void *a, void *b, void *c, void *d)
{
    wxime_log("cos_cb2 a=%p b=%p", a, b);
    return 0;
}

/* ---------------- 词典表（dbg3 于 server 0x1401a5f1e 捕获） ---------------- */
static const wxime_dict_spec k_dicts[] = {
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
#define N_DICTS ((int)(sizeof k_dicts / sizeof k_dicts[0]))

/* ---------------- InitInfo / SessionConfig 真值 ---------------- */
static char s_os_str[] = "Windows.10.0.19045.0";   /* dbg5：server 真值带 .0 后缀 */
static unsigned char s_dev_obj8[16]  = { 6,0,0,0, 3,0,0,0, 0x03,0x00,0x46,0x81, 0,0,0,0 };
static unsigned char s_guid_118[24]  = { 0x08,0x00,0x28,0x00, 0x07,0x00,0x44,0x80,
                                         0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
static unsigned long long s_obj_130[4] = { 0x5e8, 0, 0, 0 };
/* InitInfo+0x108 的 POD 真值（dbg5 解引用 dump）：{17,4}{0}{0x20000,0}{&pod2}{0}{17,1}{0} */
static unsigned long long s_pod2_108[8] = { 0x0000000100000011ULL, 0, 0, 0, 0, 0, 0, 0 };
static unsigned long long s_pod_108[8] = {
    0x0000000400000011ULL, 0,
    0x0000000000020000ULL, 0,
    (unsigned long long)s_pod2_108, 0,
    0x0000000100000011ULL, 0,
};
/* SessionConfig 312 字节真值（dbg2 断 server 0x1401a6633 捕获） */
static const unsigned char k_sc_real[WXIME_SESSIONCFG_SIZE] = {
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

/* ---------------- 引擎/会话结构 ---------------- */
struct wxime_engine {
    HMODULE dll;
    wxime_initialize_fn      initialize;
    wxime_register_cos_fn    register_cos;
    wxime_config_dict_fn     config_dict;
    wxime_create_session_fn  create_session;
    wxime_add_listener_fn    add_listener;
    wxime_warm_up_fn         warm_up;
    wxime_process_input_fn   process_input;
    wxime_get_candidates_fn  get_candidates;
    wxime_select_candidate_fn select_candidate;
    /* InitInfo 与词典资源（engine 存活期有效） */
    unsigned char ii[WXIME_INITINFO_SIZE];
    unsigned char *entries;
    char (*paths)[300];
    char *versbuf;
};

struct wxime_session {
    wxime_engine *eng;
    void *handle;
    unsigned char *inctx;
};

/* ---------------- 内部：InitInfo 填充（dbg5 真值） ---------------- */
static void fill_init_info(unsigned char *ii)
{
    memset(ii, 0, WXIME_INITINFO_SIZE);
    *(void **)(ii + 0x00) = (void *)cbA;
    *(char **)(ii + 0x18) = "Tencent\\WeType\\ImeDir";   /* workdir 真值（相对路径，
                                                           引擎按 cwd 解析） */
    *(char **)(ii + 0x40) = "13451671603782742029";      /* 20 位 deviceid 真值 */
    *(int *)(ii + 0x48) = 20;
    *(int *)(ii + 0x4c) = 2;
    *(int *)(ii + 0x50) = 6;
    *(char **)(ii + 0x68) = s_os_str;
    *(char **)(ii + 0x78) = s_os_str;
    *(int *)(ii + 0x80) = (int)sizeof s_os_str - 1;
    *(void **)(ii + 0x88) = NULL;                        /* fnB：NULL（loguru id 查询与它无关） */
    *(int *)(ii + 0xc0) = 3;
    *(void **)(ii + 0xd0) = s_fake_obj;
    *(void **)(ii + 0xd8) = s_dev_obj8;
    *(int *)(ii + 0xf0) = 0xA0;
    *(void **)(ii + 0x108) = s_pod_108;
    *(void **)(ii + 0x118) = s_guid_118;
    *(void **)(ii + 0x130) = s_obj_130;
    /* ★用户词典装配门（dbg4 于 libwxhld+0x29EB8B 实证）：server 真值
     *   +0xF8=-1、+0x100=0x7fff40000062（标记指针照抄）、+0x108→POD。
     *   全 0 会导致 config_dict 跳过 0x86 包装器构造 → create_session 崩。 */
    *(unsigned long long *)(ii + 0xF8) = (unsigned long long)-1;
    *(unsigned long long *)(ii + 0x100) = 0x7fff40000062ULL;
}

static void assemble_stub_vtable(void)
{
    s_fake_vtable[0]  = (void *)slot_copy;     s_fake_vtable[1]  = (void *)slot_copy;
    s_fake_vtable[2]  = (void *)slot_subscribe;
    s_fake_vtable[3]  = (void *)slot_type;
    s_fake_vtable[4]  = (void *)slot_destroy;
    s_fake_vtable[5]  = (void *)slot_getinner;
    s_fake_vtable[6]  = (void *)slot_misc6;
    for (int i = 7; i < 16; i++) s_fake_vtable[i] = (void *)slot_copy;
    s_fake_vtable[9]  = (void *)slot_subscribe;
    s_fake_vtable[10] = (void *)slot_type;
    s_fake_vtable[11] = (void *)slot_destroy;
    s_fake_vtable[12] = (void *)slot_getinner;
    memset(s_type_desc, 0, sizeof s_type_desc);
    s_type_desc[0] = 0xDEAD0001;
    for (int i = 0; i < 16; i++) s_inner_vtable[i] = (void *)slot_misc6;
    *(void ***)s_inner = s_inner_vtable;
    s_fake_obj[0] = (void *)s_fake_vtable;
}

/* ---------------- 公开 API ---------------- */

wxime_engine *wxime_open(const char *engine_dir, const char *client_name)
{
    (void)client_name;
    wxime_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;

    assemble_stub_vtable();
    fill_init_info(e->ii);

    e->dll = LoadLibraryA("libwxhld.dll");
    if (!e->dll) { wxime_log("LoadLibrary 失败 %lu", GetLastError()); free(e); return NULL; }
    wxime_log("libwxhld 基址 = %p（崩溃 RVA = rip-基址，IDA 地址 = 0x180000000+RVA）", (void *)e->dll);

#define BIND(field, name) \
    e->field = (wxime_##field##_fn)(uintptr_t)GetProcAddress(e->dll, name); \
    if (!e->field) { wxime_log("缺导出 %s", name); free(e); return NULL; }
    BIND(initialize,       "wxime_initialize");
    BIND(config_dict,      "wxime_config_dict");
    BIND(create_session,   "wxime_create_session");
    BIND(add_listener,     "wxime_add_session_listener");
    BIND(process_input,    "wxime_process_input");
    BIND(get_candidates,   "wxime_get_candidates");
    BIND(select_candidate, "wxime_select_candidate");
#undef BIND
    e->register_cos = (wxime_register_cos_fn)(uintptr_t)
        GetProcAddress(e->dll, "wxime_register_client_cos_api");
    e->warm_up = (wxime_warm_up_fn)(uintptr_t)
        GetProcAddress(e->dll, "wxime_warm_up_session");

    wxime_log("initialize ...");
    int r = e->initialize(e->ii);
    wxime_log("wxime_initialize -> %d", r);
    if (r != 0) { free(e); return NULL; }

    /* register_client_cos_api：server 在 initialize 后调；桩回调返回 -1 不影响 */
    if (e->register_cos) {
        unsigned long long cosapi[4] = { 0, 0,
            (unsigned long long)(uintptr_t)cos_cb1,
            (unsigned long long)(uintptr_t)cos_cb2 };
        wxime_log("register_client_cos_api -> %d", e->register_cos(cosapi));
    }

    /* config_dict：26 词典 + 版本串门（+0x10 必须是合法 MSVC 长形式
     * std::string {malloc堆指针, size, cap}——门与数据双重使用） */
    e->paths   = calloc(N_DICTS, sizeof *e->paths);
    e->entries = calloc(N_DICTS, WXIME_DICT_ENTRY_SIZE);
    e->versbuf = malloc(64);
    if (!e->paths || !e->entries || !e->versbuf) { free(e); return NULL; }
    memset(e->versbuf, 0, 64);
    e->versbuf[0] = '1';

    char base[MAX_PATH];
    if (!GetFullPathNameA(engine_dir, sizeof base, base, NULL))
        snprintf(base, sizeof base, "%s", engine_dir);
    for (int i = 0; i < N_DICTS; i++) {
        snprintf(e->paths[i], 300, "%s\\BuildIn\\%s", base, k_dicts[i].file);
        unsigned char *en = e->entries + (size_t)i * WXIME_DICT_ENTRY_SIZE;
        *(unsigned long long *)(en + 0x00) = 0;
        *(const char **)(en + 0x08) = e->paths[i];
        *(int *)(en + 0x10) = k_dicts[i].type;
        *(int *)(en + 0x14) = 0;
    }
    unsigned char cfg[WXIME_DICT_CFG_SIZE];
    memset(cfg, 0, sizeof cfg);
    *(void **)(cfg + 0x00) = e->entries;
    *(int *)(cfg + 0x08) = N_DICTS;
    *(char **)(cfg + 0x10) = e->versbuf;
    *(unsigned long long *)(cfg + 0x20) = 1;    /* size */
    *(unsigned long long *)(cfg + 0x28) = 63;   /* cap（长形式） */
    wxime_log("config_dict（%d 词典）...", N_DICTS);
    int cr = e->config_dict(cfg);
    wxime_log("wxime_config_dict -> %d", cr);

    /* 用户词典 mmap 异步构建（ImeDir/user_dict 下），server 从 init 到首会话
     * 有 ~20s 余量；直调等待，否则 Resources 的 user_coordinate_dict_ 为空 */
    wxime_log("等待用户词典异步构建(5s)...");
    Sleep(5000);
    return e;
}

/* SessionConfig 实验补丁：WXHOST_SC_PATCH="off:val[,off:val…]"（hex）——
 * 找 with_search_candidate_instantly 等布尔位（server 快照里是 0 的区段） */
static void apply_sc_patch(unsigned char *sc)
{
    const char *p = getenv("WXHOST_SC_PATCH");
    if (!p) return;
    while (*p) {
        int off = 0, val = 0; int n1 = -1;
        if (sscanf(p, "%x:%x%n", &off, &val, &n1) != 2 || n1 <= 0) break;
        if (off >= 0 && off < WXIME_SESSIONCFG_SIZE) sc[off] = (unsigned char)val;
        p += n1;
        if (*p == ',') p++;
    }
}

wxime_session *wxime_session_open(wxime_engine *eng)
{
    unsigned char sc[WXIME_SESSIONCFG_SIZE];
    memcpy(sc, k_sc_real, sizeof k_sc_real);
    apply_sc_patch(sc);
    *(const char **)(sc + 0xC0) = "wxhost";
    *(int *)(sc + 0xDC) = 5;

    wxime_log("create_session ...");
    void *h = eng->create_session(sc);
    wxime_log("session = %p", h);
    if (!h) return NULL;

    wxime_session *s = calloc(1, sizeof *s);
    s->eng = eng;
    s->handle = h;
    s->inctx = calloc(1, WXIME_INPUT_CTX_SIZE);

    wxime_event_attach(h, eng->get_candidates);
    if (eng->warm_up) { wxime_log("warm_up ..."); eng->warm_up(h); }
    return s;
}

int wxime_send_key(wxime_session *s, const char *utf8_key, int len)
{
    /* 引擎会话线程可能还在上一键的事件回调里（726 候选枚举毫秒级）——
     * 撞进去会崩（nihaoma 7 键连发实测）。等 listener 空闲再发。 */
    for (int i = 0; i < 100 && !wxime_event_listener_idle(); i++) Sleep(2);
    /* r9 必须传堆上 context/out 缓冲（传 NULL 会穿空指针崩 vmovdqu） */
    memset(s->inctx, 0, WXIME_INPUT_CTX_SIZE);
    return s->eng->process_input(s->handle, utf8_key, len, s->inctx, WXIME_MAGIC_10);
}

int wxime_select(wxime_session *s, const void *cand_tlv, const char *word)
{
    /* select 前等 listener 空闲 + 短暂喘息（对照 probe3 的时序余量；
     * 不等的话引擎事件分发线程会静默死亡——select 后再无任何事件） */
    for (int i = 0; i < 100 && !wxime_event_listener_idle(); i++) Sleep(2);
    Sleep(100);
    int wl = word ? (int)strlen(word) : 0;
    if (!cand_tlv || wl <= 0) return -1;
    if (wl > 15) wl = 15;   /* SSO 缓冲 16 字节 */

    /* ★sst/idbuf 必须 static（引擎异步消费：select 投任务后会话线程才读
     * sst——栈缓冲函数返回即回收，会话线程读到垃圾后事件分发线程静默
     * 死亡，之后所有按键零事件。probe3 用 static 恰好存活，实证此差异）。 */
    /* ★id = 候选 TLV 前 100 字节原样序列化（管道侧 hex 实证，结构与
     * ci+0x20 的 TLV 完全同构：02|tag|idx|音节数|[len]拼音[len]词…|float|
     * 音节表）。栈参 5 是 64 位：低 32=idlen(100)，高 32=标志 1（server
     * 现场 [rsp+0x20]={100,1} 同槽；传纯 int 100 高位为 0 → 引擎按整段
     * commit，剩余组词不推——部分选词全靠这个标志）。 */
    static unsigned char idbuf[128];
    memset(idbuf, 0, sizeof idbuf);
    int idlen = 0;
    if (wx_readable((void *)cand_tlv, 100)) {
        idlen = 100;
        memcpy(idbuf, cand_tlv, 100);
    }
    static unsigned char sst[WXIME_SELECT_CTX_SIZE];
    long long outv[4] = { 0, 0, 0, 0 };
    memset(sst, 0, sizeof sst);
    memcpy(sst, word, (size_t)wl);
    *(unsigned long long *)(sst + 0x10) = wl;      /* std::string size */
    *(unsigned long long *)(sst + 0x18) = 0xF;     /* SSO cap */
    *(void **)(sst + 0x20) = (void *)cand_tlv;
    *(unsigned long long *)(sst + 0x30) = 100;
    *(unsigned long long *)(sst + 0x38) = 0x6F;

    int sr = s->eng->select_candidate(s->handle, sst, wl, (void *)cand_tlv,
                                      ((unsigned long long)idlen) | (1ULL << 32),
                                      idbuf, 0, outv, WXIME_MAGIC_10, NULL);
    wxime_log("select_candidate(word=\"%s\") -> %d", word, sr);
    return sr;
}

int wxime_warm_up(wxime_session *s)
{
    return s->eng->warm_up ? s->eng->warm_up(s->handle) : 0;
}

/* ---------------- 用户热词（学习强化） ----------------
 * wxime_set_user_hot_word（RVA 1ebd20，官方 server 实调）：参数=结构指针，
 * 3×{char* ptr, int len} 对（+0x00/+0x08、+0x10/+0x18、+0x20/+0x28，每对
 * 16B 步进），返回 BOOL。server 调用现场：字段1 空(NULL,0)，字段2/3 取自
 * 词项对象 +0x28/+0x20 的两个 SSO 串。串序语义（词 vs 拼音）静态分析不
 * 定，实验定：word 与 pinyin 两种顺序都试过（wxhost 命令 HW）。 */
typedef int (*wxime_set_hotword_fn)(const void *hw);

int wxime_set_hot_word(wxime_engine *eng, const char *s2, int s2len,
                       const char *s3, int s3len)
{
    wxime_set_hotword_fn fn =
        (wxime_set_hotword_fn)(uintptr_t)GetProcAddress(eng->dll,
                                                        "wxime_set_user_hot_word");
    if (!fn) return -1;
    /* static：引擎内部异步消费可能性（同 select 的 sst 教训） */
    static struct {
        const char *p1; int l1; int pad1;
        const char *p2; int l2; int pad2;
        const char *p3; int l3; int pad3;
        unsigned long long tail[4];
    } hw;
    memset(&hw, 0, sizeof hw);
    hw.p2 = s2; hw.l2 = s2len;
    hw.p3 = s3; hw.l3 = s3len;
    int r = fn(&hw);
    wxime_log("set_user_hot_word(s2=\"%s\",s3=\"%s\") -> %d", s2 ? s2 : "", s3 ? s3 : "", r);
    return r;
}

/* ---------------- 用户历史（联想数据源，2026-10-21 续挖） ----------------
 * wxime_add_user_history(词指针, 词字节数≤24)（IDA 伪代码定论）：
 * 日志串 "add user dict word len: %u"；流程 = 在用户词典查该词记录，
 * 恰一条时取记录 SSO 串带 {type=5, 权重1.0f} 提交进历史——即"记录一次
 * 用户选词"。查不到记录返回 0（安全路径，无崩溃分支）。select 内部
 * 学习只写词频，句子历史（sequel 联想的上文依赖）只有本 API 写。 */
typedef int (*wxime_add_history_fn)(const void *, unsigned int);

int wxime_add_history(wxime_session *s, const char *word)
{
    wxime_add_history_fn fn =
        (wxime_add_history_fn)(uintptr_t)GetProcAddress(
            s->eng->dll, "wxime_add_user_history");
    if (!fn) return -1;
    int wl = word ? (int)strlen(word) : 0;
    if (wl <= 0 || wl > 24) return -1;
    /* static：引擎内部可能异步读（同 select 的 sst 教训） */
    static char wbuf[32];
    memset(wbuf, 0, sizeof wbuf);
    memcpy(wbuf, word, (size_t)wl);
    for (int i = 0; i < 100 && !wxime_event_listener_idle(); i++) Sleep(2);
    int r = fn(wbuf, (unsigned int)wl);
    wxime_log("add_user_history(\"%s\") -> %d", wbuf, r);
    return r;
}

/* ---------------- 联想（get_most_likely_sequel） ----------------
 * 官方 server 两处调用形态合成：
 *   ① set_text_around_cursor(session, before,beforeLen, sel,selLen,
 *        after,afterLen, int)（1401af64a 现场：rdx/r8d、r9/栈0x20、
 *        栈0x28/0x30 三对 {ptr,len} + 栈0x38 int）
 *   ② set_bool_option(session, 0xE, 1) 开联想开关（1401aeed0 适配层）
 *   ③ get_most_likely_sequel(session)（返回 BOOL，结果异步推事件） */
typedef int (*wxime_set_text_around_fn)(void *, const char *, int,
                                        const char *, int,
                                        const char *, int, int);
typedef int (*wxime_set_bool_opt_fn)(void *, int, unsigned char);
typedef int (*wxime_sequel_fn)(void *);
typedef int (*wxime_force_emit_fn)(void *, const char *, int, int);

int wxime_query_sequel(wxime_session *s, const char *before)
{
    HMODULE dll = s->eng->dll;
    wxime_set_text_around_fn txt =
        (wxime_set_text_around_fn)(uintptr_t)GetProcAddress(dll, "wxime_set_text_around_cursor");
    wxime_set_bool_opt_fn opt =
        (wxime_set_bool_opt_fn)(uintptr_t)GetProcAddress(dll, "wxime_session_set_bool_option");
    wxime_sequel_fn seq =
        (wxime_sequel_fn)(uintptr_t)GetProcAddress(dll, "wxime_get_most_likely_sequel");
    wxime_force_emit_fn emit =
        (wxime_force_emit_fn)(uintptr_t)GetProcAddress(dll, "wxime_force_emit_pending_input");
    if (!txt || !opt || !seq) return -1;
    for (int i = 0; i < 100 && !wxime_event_listener_idle(); i++) Sleep(2);
    int bl = before ? (int)strlen(before) : 0;
    int t = txt(s->handle, before, bl, "", 0, "", 0, 0);
    opt(s->handle, 0xE, 1);
    wxime_event_expect_sequel(before);   /* 响应事件锚定解析（见 wxime_event.c） */
    int r = seq(s->handle);
    /* ★定论（IDA + 实验）：sequel 完成回调 = session->vt[0x40]，自行把
     * 联想词表作为组词事件推 listener（实测"你爸"等词表到达）——无需
     * force_emit。force_emit(session,str,len,1) 实测必崩（引擎内解引用
     * -1），且崩溃杀死主线程；bool=0 则无事件。serve 用组词表分支收。 */
    (void)emit;
    Sleep(50);   /* 引擎异步分发喘息 */
    wxime_log("text_around(\"%s\")->%d sequel->%d", before ? before : "", t, r);
    return r;
}

/* ★立即搜索（wxime_search_candidates(session)→bool，1eacc0 反汇编：单参、
 * 向引擎单例虚调用 [vt+0x20] 投搜索请求）。引擎对 4+ 键简拼不再主动推
 * 新候选迭代器（fccdjny 实测第 3 键起），本调用强制搜索当前组词——
 * Android 侧 with_search_candidate_instantly 会话包装的手动版。 */
int wxime_search_now(wxime_session *s)
{
    typedef int (*fn_t)(void *);
    fn_t f = (fn_t)(uintptr_t)GetProcAddress(s->eng->dll, "wxime_search_candidates");
    if (!f) return -1;
    for (int i = 0; i < 100 && !wxime_event_listener_idle(); i++) Sleep(2);
    int r = f(s->handle);
    wxime_log("search_candidates -> %d", r);
    return r;
}

int wxime_session_reset(wxime_session *s)
{
    wxime_destroy_session_fn destroy =
        (wxime_destroy_session_fn)(uintptr_t)GetProcAddress(
            s->eng->dll, "wxime_destroy_session");
    if (destroy && s->handle) {
        wxime_log("destroy_session(%p)", s->handle);
        destroy(s->handle);
    }
    unsigned char sc[WXIME_SESSIONCFG_SIZE];
    memcpy(sc, k_sc_real, sizeof k_sc_real);
    apply_sc_patch(sc);
    *(const char **)(sc + 0xC0) = "wxhost";
    *(int *)(sc + 0xDC) = 5;
    wxime_log("create_session（重置）...");
    s->handle = s->eng->create_session(sc);
    wxime_log("session = %p", s->handle);
    if (!s->handle) return -1;
    wxime_event_attach(s->handle, s->eng->get_candidates);
    if (s->eng->warm_up) s->eng->warm_up(s->handle);
    return 0;
}
