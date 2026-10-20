/* wetype Windows 引擎探针 (Phase 0)
 * 在 wine 里 LoadLibrary(libwxhld.dll)，按 vtype harness/engine.c 的
 * ARM64 实测布局调 wxime_initialize，验证 Windows x64 引擎可否驱动。
 * 编译: zig cc -target x86_64-windows-gnu -O1 -o probe.exe probe.c
 *   或: x86_64-w64-mingw32-gcc -static -O1 -o probe.exe probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* InitInfo 布局（十进制字节偏移，基址=传入指针），迁移自 engine.c 的 ARM64 实测：
 * +0   int platform
 * +16  引擎读取，语义未明（保持 0）
 * +24  char* 工作目录（引擎自动建目录；引擎会 free，必须堆分配）
 * +48/64/88/104/120  {char* data, int len} 候选对，暴力全填 uin
 * +168 指向容器对象的指针 {?, ?, data@+16, count@+24}，填空容器则跳过遍历
 * +184/+188  float，填 1.0
 * 总长 384
 */
#define OFF_PLATFORM 0
#define OFF_WORKDIR 24
#define OFF_CONTAINER 168
#define OFF_FLOAT_A 184
#define OFF_FLOAT_B 188
#define STRUCT_SIZE 384

typedef int (*fn_init_t)(void *);
typedef int (*fn_uid_t)(const char *, int);

/* 引擎可能 free 容器内容，堆分配一份空容器最稳妥 */
static unsigned char *empty_container;

/* 回调桩：引擎在 initialize 里调用结构体偏移0处的函数指针（实测 x64 布局与
 * ARM64 的 engine.c 不同：+0 是回调而非 int platform）。打进来看参数要什么。 */
static void dump_mem(const char *tag, void *p)
{
    if (!p || IsBadReadPtr(p, 32)) { printf("[cb0] %s=%p <unreadable>\n", tag, p); return; }
    unsigned char *b = (unsigned char *)p;
    printf("[cb0] %s=%p:", tag, p);
    for (int i = 0; i < 32; i++) printf(" %02X", b[i]);
    printf("  |");
    for (int i = 0; i < 32; i++) printf("%c", (b[i] >= 0x20 && b[i] < 0x7F) ? b[i] : '.');
    printf("|\n");
}

static long long cb0(void *a, void *b, void *c, void *d)
{
    printf("[cb0] a=%p b=%p c=%p d=%p\n", a, b, c, d);
    dump_mem("b", b);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dll = argc > 1 ? argv[1] : "libwxhld.dll";
    const char *workdir = argc > 2 ? argv[2] : "Z:\\wetype-work";
    int platform = argc > 3 ? atoi(argv[3]) : 0;

    SetErrorMode(SEM_FAILCRITICALERRORS);
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("LoadLibrary FAILED err=%lu\n", GetLastError()); return 1; }
    printf("LoadLibrary ok: %s\n", dll);
    fflush(stdout);

    fn_uid_t p_uid = (fn_uid_t)GetProcAddress(h, "wxime_reset_user_id");
    fn_init_t p_init = (fn_init_t)GetProcAddress(h, "wxime_initialize");
    printf("wxime_reset_user_id @ %p\n", (void *)p_uid);
    printf("wxime_initialize      @ %p\n", (void *)p_init);
    fflush(stdout);
    if (!p_init) { printf("no wxime_initialize\n"); return 2; }

    const char *uin = "10000001";
    int no_uid = argc > 4 && !strcmp(argv[4], "no-uid");
    if (p_uid && !no_uid) {
        int r = p_uid(uin, (int)strlen(uin));
        printf("wxime_reset_user_id -> %d\n", r);
        fflush(stdout);
    } else if (no_uid) {
        printf("(skip wxime_reset_user_id)\n");
        fflush(stdout);
    }

    unsigned char *buf = (unsigned char *)calloc(1, 1024); /* x64 布局未知，加大余量 */
    *(void **)(buf + 0) = (void *)cb0;                     /* 实测 +0 为回调指针 */
    *(char **)(buf + OFF_WORKDIR) = _strdup(workdir);

    static const int pair_offs[] = {48, 64, 88, 104, 120};
    for (unsigned i = 0; i < sizeof(pair_offs) / sizeof(pair_offs[0]); i++) {
        char *p = _strdup(uin);
        *(char **)(buf + pair_offs[i]) = p;
        *(int *)(buf + pair_offs[i] + 8) = (int)strlen(uin);
    }

    empty_container = (unsigned char *)calloc(1, 32);
    *(void **)(buf + OFF_CONTAINER) = empty_container;
    *(float *)(buf + OFF_FLOAT_A) = 1.0f;
    *(float *)(buf + OFF_FLOAT_B) = 1.0f;

    printf("calling wxime_initialize(platform=%d, workdir=%s)...\n", platform, workdir);
    fflush(stdout);
    int r = p_init(buf);
    printf("wxime_initialize -> %d (0x%x)\n", r, r);
    fflush(stdout);
    return r == 0 ? 0 : 3;
}
