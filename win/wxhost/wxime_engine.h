/* wxime_engine.h —— 引擎生命周期与输入/选词 API（主线程调用）
 *
 * 用法：
 *   wxime_engine *eng = wxime_open(".", "wxhost");        // 加载+初始化+词典+等待
 *   wxime_session *sess = wxime_session_open(eng);         // 建会话+挂 listener
 *   wxime_send_key(sess, "n", 1);                          // 逐键
 *   ... wxime_poll_candidates() 取候选（wxime_event.h）...
 *   wxime_select(sess, &cand);                             // 选词（上屏文本= cand.word）
 *
 * 引擎日志经 cbA 回流时打印到 stdout。
 */
#ifndef WXIME_ENGINE_H
#define WXIME_ENGINE_H

#include "wxime_abi.h"

typedef struct wxime_engine wxime_engine;
typedef struct wxime_session wxime_session;

/* 统一日志（各模块共用，前缀 [wxhost]） */
void wxime_log(const char *fmt, ...);
void wxime_log_to_stderr(void);   /* serve 模式：stdout 是协议通道，日志全走 stderr */

/* 无副作用可读检查（替代 IsBadReadPtr）：wine 的 IsBadReadPtr 对部分坏
 * 指针自身 AV（组词元素=词串字节值时中招），异常穿透杀死引擎分发线程。
 * VirtualQuery 纯查询，逐页验 MEM_COMMIT 且非 PAGE_NOACCESS/GUARD。 */
int wx_readable(const void *p, size_t n);

/* 加载 libwxhld.dll 并完成完整初始化序列：
 * initialize(InitInfo 真值) → register_client_cos_api(桩) → config_dict(26 词典
 * + 版本串门) → 等待用户词典异步构建(5s)。engine_dir 为引擎目录（BuildIn/ 所在），
 * 通常传 "."。失败返回 NULL（原因已打印）。 */
wxime_engine *wxime_open(const char *engine_dir, const char *client_name);

/* 创建会话（SessionConfig 312B 真值回放 + 客户端名）并挂事件 listener。
 * 失败返回 NULL。 */
wxime_session *wxime_session_open(wxime_engine *eng);

/* 逐键输入（UTF-8 编码的按键字符串，len=字节数）。返回引擎结果（1=已处理）。 */
int wxime_send_key(wxime_session *sess, const char *utf8_key, int len);

/* 选词。cand 来自 wxime_poll_candidates 的快照（TLV 指针须未被下一轮候选
 * 事件覆盖）。返回引擎结果（1=成功）。上屏文本即 cand->word（宿主自行提交
 * 给应用，同 Route W jinterop 契约）。 */
int wxime_select(wxime_session *sess, const void *cand_tlv, const char *word);

/* 预热（可选，server 在挂 listener 后调用） */
int wxime_warm_up(wxime_session *sess);

/* 设置用户热词（学习强化入口；s2/s3 两串顺序语义见实现注释，实验定序） */
int wxime_set_hot_word(wxime_engine *eng, const char *s2, int s2len,
                       const char *s3, int s3len);

/* 联想：设上文文本 + 开 14 号开关 + get_most_likely_sequel，结果经事件管线 */
int wxime_query_sequel(wxime_session *sess, const char *before);

/* 强制引擎立即搜索当前组词（4+ 键引擎不主动推候选时用） */
int wxime_search_now(wxime_session *sess);

/* 记录一次用户选词历史（联想数据源；词 UTF-8 ≤24 字节） */
int wxime_add_history(wxime_session *sess, const char *word);

/* 会话重置（C 命令）：销毁当前会话并重建（ESC 清不掉候选展示状态，
 * 唯一可靠的重置方式，同 Route W 断线重连语义）。listener 自动重挂。
 * 返回 0=成功。 */
int wxime_session_reset(wxime_session *sess);

#endif /* WXIME_ENGINE_H */
