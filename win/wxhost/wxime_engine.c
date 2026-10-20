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

/* ---------------- 日志 ---------------- */
void wxime_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[wxhost] "); vprintf(fmt, ap); printf("\n"); fflush(stdout);
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

/* ---------------- 引擎日志回调（InitInfo+0x00，loguru sink） ---------------- */
static long long cbA(void *a, void *b, void *c, void *d)
{
    if (b && !IsBadReadPtr(b, 200)) {
        char buf[201]; int n = 0;
        for (char *p = (char *)b; *p && n < 200; p++, n++) buf[n] = *p;
        buf[n] = 0;
        if (n > 0) printf("[engine] %s\n", buf), fflush(stdout);
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

wxime_session *wxime_session_open(wxime_engine *eng)
{
    unsigned char sc[WXIME_SESSIONCFG_SIZE];
    memcpy(sc, k_sc_real, sizeof k_sc_real);
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
    if (!IsBadReadPtr((void *)cand_tlv, 100)) {
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
