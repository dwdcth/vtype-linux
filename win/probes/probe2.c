/* probe2.c v2 —— 路线 C（单 exe 直调）会话打通探针
 *
 * 相比 v1 的三处关键升级：
 * 1. InitInfo 严格按 dbg.c 捕获的 wetype_server 真值表（win/README §3.7 与
 *    docs/上游/windows-engine-route-notes.md）：
 *    +0x00 fnA(日志回调) +0x18 堆workdir +0x40 deviceid(+0x48 len,+0x4c 2)
 *    +0x50 platform=6 +0x68/0x78 "Windows.10.0.19045"(+0x80 len 0x14)
 *    +0x88 fnB(第二回调，initialize 会 call [a1+0x88]！) +0xc0 int 3
 *    +0xd0 接口对象(本次桩化) +0xd8 {6,3,0x81460003} +0xf0 0xA0
 *    +0x108 {11,0,1,0} +0x118 GUID样 +0x130 {0x5e8,0}
 * 2. +0xd0 桩对象：16 槽日志虚表。虚表语义（server 0x140563640 实测）：
 *    slot0/1 copy(out)→写{vtable,payload}返回out  slot2/9 订阅(引擎move回调对象进来,
 *    暂存)  slot3/10 返回类型描述符  slot4/11 条件析构(只log不free)
 *    slot5/12 返回this+8(内层对象=独立桩)  slot6 数据非代码(不会调,log兜底)
 * 3. SEH __try/__except 捕获崩溃：打印崩溃 RIP+最近调用槽位，crash-by-crash 收敛。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void *(*fn_create_session_t)(void *);
typedef void (*fn_add_listener_t)(void *, void *, void *);
typedef int  (*fn_process_input_t)(void *, const char *, int, void *);

/* ---------------- 日志与崩溃现场 ---------------- */
static volatile LONG g_last_slot = -1;
static void slog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[probe2] "); vprintf(fmt, ap); printf("\n"); fflush(stdout);
    va_end(ap);
}

/* ---------------- 16 槽桩虚表 ---------------- */
static long long __fastcall slot_copy(void *self, void **out)   /* slot0/1/7/8 */
{
    slog("slot COPY self=%p out=%p", self, out);
    out[0] = *(void **)self;      /* 我们的 vtable */
    out[1] = (void *)0x1111;      /* payload（真实现是 inner 指针，先放哨兵） */
    return (long long)out;
}
static long long __fastcall slot_subscribe(void *self, void **moved) /* slot2/9 */
{
    void *obj = moved[0];
    slog("slot SUBSCRIBE(引擎注册回调对象) self=%p obj=%p", self, obj);
    moved[0] = NULL;              /* move 语义：清空源 */
    if (obj) {
        if (!IsBadReadPtr(obj, 16)) {
            void **vt = *(void ***)obj;
            slog("  对象vtable=%p slot0=%p slot1=%p slot2=%p",
                 vt, vt[0], vt[1], vt[2]);
        }
        /* 暂存：引擎稍后可能期望我们触发它；不 free */
        static void *g_sink; g_sink = obj;
    }
    return 0;
}
static void *g_type_desc;         /* slot3/10 返回的描述符块 */
static long long __fastcall slot_type(void *self, void *unused) /* slot3/10 */
{
    slog("slot TYPEDESC self=%p", self);
    return (long long)g_type_desc;
}
static long long __fastcall slot_destroy(void *self, char cond)  /* slot4/11 */
{
    slog("slot DESTROY self=%p cond=%d (桩不释放)", self, cond);
    return 0;
}
static char g_inner[64];          /* 内层对象体（带自己的桩虚表） */
static long long __fastcall slot_getinner(void *self)            /* slot5/12 */
{
    slog("slot GETINNER self=%p -> %p", self, g_inner);
    return (long long)g_inner;
}
static long long __fastcall slot_misc6(void *a, void *b, void *c, void *d) /* 其余 */
{
    InterlockedExchange(&g_last_slot, 6);
    slog("slot MISC6 a=%p b=%p c=%p d=%p", a, b, c, d);
    return 0;
}

static void *g_fake_vtable[16];

/* 内层对象虚表：全日志桩 */
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
static long long cbListener(void *a, void *b, void *c, void *d, void *e, void *f)
{
    slog("LISTENER a=%p b=%p c=%p d=%p", a, b, c, d);
    if (a && !IsBadReadPtr(a, 48)) {
        unsigned char *x = (unsigned char *)a;
        slog("  a: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
             x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7],
             x[8],x[9],x[10],x[11],x[12],x[13],x[14],x[15]);
    }
    return 0;
}

/* ---------------- InitInfo / SessionConfig ---------------- */
static unsigned char ii[0x138];
static unsigned char sc[0x200];
static char os_str[] = "Windows.10.0.19045";
static unsigned char dev_obj8[16]  = { 6,0,0,0, 3,0,0,0, 0x03,0x00,0x46,0x81, 0,0,0,0 };
static unsigned int  obj_108[4]    = { 11, 0, 1, 0 };
static unsigned char guid_118[24]  = { 0x08,0x00,0x28,0x00, 0x07,0x00,0x44,0x80,
                                       0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
static unsigned long long obj_130[4] = { 0x5e8, 0, 0, 0 };


/* VEH 崩溃捕获：打 RIP 与最近桩槽位（clang 无 __try，用向量异常处理器） */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    printf("[probe2] !! AV 崩溃 rip=%llu fault=%llu last_slot=%d\n",
           (unsigned long long)ep->ContextRecord->Rip,
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
           g_last_slot);
    printf("[probe2] InitInfo 现场:\n");
    for (int row = 0; row < 0x138; row += 16) {
        unsigned long long *q = (unsigned long long *)(ii + row);
        printf("  +%03x: %016llx %016llx\n", row, q[0], q[1]);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;   /* 终止进程，现场已在上面打出 */
}

int main(int argc, char **argv)
{
    const char *engine_dir = argc > 1 ? argv[1] : ".";
    char p_work[300];
    snprintf(p_work, sizeof p_work, "%s\\work", engine_dir);
    CreateDirectoryA(p_work, NULL);

    /* 桩虚表装配（slot 编号对应 server 0x140563640 实测布局） */
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
    /* 描述符块：仿 server 0x1406ef180 首域（指针+零） */
    static unsigned long long desc[8];
    memset(desc, 0, sizeof desc);
    desc[0] = 0xDEAD0001;   /* 哨兵：引擎若解引用会崩在可识别地址 */
    g_type_desc = desc;
    /* 内层对象：自己的桩虚表 */
    static void *inner_vt[16];
    for (int i = 0; i < 16; i++) inner_vt[i] = (void *)inner_stub;
    *(void ***)g_inner = inner_vt;

    /* InitInfo（+0xd0 桩对象本体：{vtable_ptr}） */
    static void *fake_obj[4];
    fake_obj[0] = (void *)g_fake_vtable;

    memset(ii, 0, sizeof ii);
    *(void **)(ii + 0x00) = (void *)cbA;
    *(char **)(ii + 0x18) = _strdup(p_work);
    *(char **)(ii + 0x40) = "1345167160378274";  /* 16位设备id（真值20位，先短填） */
    *(int *)(ii + 0x48) = 16;
    *(int *)(ii + 0x4c) = 2;
    *(int *)(ii + 0x50) = 6;                      /* platform=Windows */
    *(char **)(ii + 0x68) = os_str;
    *(char **)(ii + 0x78) = os_str;
    *(int *)(ii + 0x80) = (int)sizeof os_str - 1; /* 0x14 */
    *(void **)(ii + 0x88) = (void *)cbB;
    *(int *)(ii + 0xc0) = 3;
    *(void **)(ii + 0xd0) = fake_obj;             /* ★ 桩对象 */
    *(void **)(ii + 0xd8) = dev_obj8;
    *(int *)(ii + 0xf0) = 0xA0;
    *(void **)(ii + 0x108) = obj_108;
    *(void **)(ii + 0x118) = guid_118;
    *(void **)(ii + 0x130) = obj_130;

    /* SessionConfig */
    memset(sc, 0, sizeof sc);
    *(unsigned char *)(sc + 0x01) = 1;
    *(int *)(sc + 0x04) = 0;                      /* 全拼 */
    *(unsigned char *)(sc + 0x125) = 1;
    *(unsigned short *)(sc + 0x127) = 0x0101;

    AddVectoredExceptionHandler(1, crash_veh);
    HMODULE h = LoadLibraryA("libwxhld.dll");
    if (!h) { slog("LoadLibrary 失败 %lu", GetLastError()); return 1; }
    slog("dll loaded");

    int (*initialize)(void *) = (int (*)(void *))GetProcAddress(h, "wxime_initialize");
    fn_create_session_t create_session = (fn_create_session_t)GetProcAddress(h, "wxime_create_session");
    fn_add_listener_t add_listener = (fn_add_listener_t)GetProcAddress(h, "wxime_add_session_listener");
    fn_process_input_t process_input = (fn_process_input_t)GetProcAddress(h, "wxime_process_input");
    int (*warm_up)(void *) = (int (*)(void *))GetProcAddress(h, "wxime_warm_up_session");

    slog("initialize...");
    int r = initialize(ii);
    slog("wxime_initialize -> %d", r);
    if (r != 0) return 2;

    slog("create_session...");
    void *sess = create_session(sc);
    slog("session = %p", sess);
    if (!sess) return 3;

    add_listener(sess, (void *)cbListener, NULL);
    slog("listener 已挂");
    if (warm_up) { slog("warm_up..."); warm_up(sess); }
    for (const char *k = "nihao"; *k; k++) {
        slog("input '%c'", *k);
        int pr = process_input(sess, k, 1, NULL);
        slog("  -> %d", pr);
        Sleep(300);
    }
    Sleep(2000);
    slog("done");
    return 0;
}
