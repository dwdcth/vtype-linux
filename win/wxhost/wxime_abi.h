/* wxime_abi.h —— libwxhld.dll（微信输入法 Windows 引擎，mozc 魔改 x86-64）
 * 的 C ABI 声明：导出函数指针类型 + 结构布局常量 + 词典表数据类型。
 *
 * 全部偏移为 2026-10 动态实证（dbg 系列断点 + objdump），出处见
 * win/README.md §6.0。libwxhld 导出 142 个 wxime_*（win/ref/exports_rva.txt），
 * 本头只声明已打通的子集，其余按需追加。
 */
#ifndef WXIME_ABI_H
#define WXIME_ABI_H

#include <windows.h>

/* ---------------- 导出函数指针类型 ---------------- */

typedef int  (*wxime_initialize_fn)(void *init_info);
typedef int  (*wxime_register_cos_fn)(void *cos_api);
typedef int  (*wxime_config_dict_fn)(void *cfg);
typedef void *(*wxime_create_session_fn)(const void *session_config);
typedef void (*wxime_add_listener_fn)(void *session, void *listener, void *ctx);
typedef int  (*wxime_warm_up_fn)(void *session);
typedef int  (*wxime_process_input_fn)(void *session, const char *key,
                                       int len, void *ctx, int flag);
typedef int  (*wxime_get_candidates_fn)(void *iter, void *cb, void *user);
typedef int  (*wxime_select_candidate_fn)(void *session, const void *selctx,
                                          int word_len, void *tlv,
                                          unsigned long long idlen_flag,
                                          void *id, int zero, void *out,
                                          int magic, void *desc);
/* idlen_flag：低 32 位 = id 字节长（100），高 32 位 = 标志 1（server 现场
 * 实证——高位为 0 时引擎按整段 commit，剩余组词不推）。 */
typedef int  (*wxime_destroy_session_fn)(void *session);
typedef int  (*wxime_reset_session_fn)(void *session);

/* ---------------- 结构尺寸与关键偏移 ---------------- */

#define WXIME_INITINFO_SIZE   0x138  /* wxime_config C++ 对象（initialize 入参） */
#define WXIME_SESSIONCFG_SIZE 0x138  /* create_session 入参（312B 真值回放） */
#define WXIME_DICT_CFG_SIZE   0x30   /* config_dict 入参 */
#define WXIME_DICT_ENTRY_SIZE 0x18   /* 词典条目 {qword0, char* 路径, int type, int 0} */

/* 候选事件（LISTENER 的 b 参数）：
 *   类型门：*(b+0x10)==b（双接口自引用）且 *(b+0x08)!=0 才是候选更新事件，
 *   其余事件（组词等）的 +0x30 区是 tagged/栈残值，枚举必崩。
 *   iterator 位于 b+0x90（引擎日志 malloc new_iterator 与 b 偏移恒 0x90）。 */
#define WXIME_EVENT_SELFREF_OFF 0x10
#define WXIME_EVENT_PAYLOFF_OFF 0x08
#define WXIME_ITER_OFF          0x90

/* candidate_info（get_candidates 回调入参 ci，每次复用同一栈槽，须当场取走）：
 *   ci+0x20 → TLV 对象：`02 | tag(4B) | 候选序号 | [len]拼音 [len]词 [len]词(重复)
 *   … 附属段`（长度前缀 UTF-8，串区从 +0x1C 起，可能带 [01][n] 计数前缀）
 *   ci+0x38 → JSON 全量报告（C 字符串直读，records[].prefix/type） */
#define WXIME_CI_TLV_OFF  0x20
#define WXIME_CI_JSON_OFF 0x38
#define WXIME_TLV_STR_BASE 0x1C

/* wxime_get_candidates 迭代协议（反汇编 RVA 1e2d30 实证）：
 *   同步 for 循环调 cb(ci, user)；cb 返回 BOOL——非零=继续、零=停止。
 *   get_candidates 自身返回 1=成功 0=失败。 */
typedef int (*wxime_cand_cb_fn)(void *ci, void *user);

/* wxime_select_candidate 十参 ABI（dbg8 断 server 0x1401a3ceb 实证）：
 *   selctx = 0x40 字节 {+0x00 SSO 词串, +0x10 size, +0x18 0xF(SSO cap),
 *   +0x20 候选 TLV 指针, +0x30 idlen 槽, +0x38 0x6F}；
 *   tlv    = 候选 TLV 对象指针（枚举时从 ci+0x20 捕获，引擎原样回收）；
 *   id     = 零缓冲即可（server 传的 id 内存实测全 0，引擎不校验内容）；
 *   magic  = 0x10（与 process_input 尾参同族的客户端类型魔数）；
 *   desc   = NULL 可（server 传的 desc 实测已是释放内存）。
 *   ★只能在主线程调：listener（引擎异步分发线程）内调用会向会话线程
 *   投任务并等待 → 死锁。 */
#define WXIME_SELECT_CTX_SIZE 0x40
#define WXIME_MAGIC_10        0x10

/* process_input：r9 必须传堆上 context/out 缓冲（server 传 malloc 出的
 * 缓冲，内含客户端名等；传 NULL 在 vmovdqu(%rdx=0) 崩），尾参恒 0x10。 */
#define WXIME_INPUT_CTX_SIZE 0x100

/* ---------------- 词典表（dbg3 断 server 0x1401a5f1e 捕获的真值） ----------------
 * type 与 BuildIn/index.json 的 type 字段一致（29 个里排除 3 个 swbd* LM）。
 * 官方升级时按 win/README.md「升级官方版本」节重新核对。 */
typedef struct { int type; const char *file; } wxime_dict_spec;

#endif /* WXIME_ABI_H */
