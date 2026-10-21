/* wxime_event.h —— 事件侧 API：候选快照轮询
 *
 * listener 跑在引擎异步分发线程上：里面只做同步候选枚举（get_candidates）
 * 填快照，绝不调用 select 等会向会话线程投任务的 API（死锁）。
 * 主线程每键后 wxime_poll_candidates 取走快照。
 */
#ifndef WXIME_EVENT_H
#define WXIME_EVENT_H

#include "wxime_abi.h"

#define WXIME_SNAP_MAX 512   /* 单事件最多 726 个候选，快照截断到 512（计数保留） */

/* 候选快照（listener 枚举时从 candidate_info 现场解码——ci 复用同一栈槽，
 * 必须当场取走，故存副本而非指针。tlv 指针选词要用，存活到下一轮候选事件。） */
typedef struct {
    char pinyin[32];   /* UTF-8，如 "ni,hao" */
    char word[64];     /* UTF-8，如 "你好" */
    char type[16];     /* 候选类型（ci+0x38 JSON 的 records[0].type）：
                          "exact_match" 可选；"completion" 选取会引引擎
                          C++ 异常（实证），S 必须拒绝 */
    void *tlv;         /* 引擎候选 TLV 对象（select 凭证） */
} wxime_candidate;

/* 挂 listener（wxime_session_open 内部调用） */
void wxime_event_attach(void *session, wxime_get_candidates_fn get_candidates);

/* 主线程：取走最近一次候选更新事件的候选表。
 * 返回候选数（0=自上次取走后无候选更新），拷贝进 out（至多 max 个）。 */
int wxime_poll_candidates(wxime_candidate *out, int max);

/* 活迭代器重枚举（主线程）：引擎 3+ 键不再推候选事件时，用 listener
 * 存下的活迭代器重读当前候选。返回候选数，-1=无迭代器/并发忙。 */
int wxime_event_refresh(void);

/* 测试并清零组词事件脏标志：真=引擎状态变过（3+ 键无候选事件的信号） */
int wxime_event_state_dirty(void);

/* 标记即将发起 sequel：期间到达的非门事件按候选事件枚举（联想结果
 * 走 CandidateUpdatedEvent 但不满足自引用门——见 wxime_event.c） */
void wxime_event_expect_sequel(const char *anchor);

/* 清全部待取快照（残留旧表防误读） */
void wxime_event_clear_all(void);

/* 取走联想词表快照（sequel 结果，无选词凭证，展示/本地拼词用） */
int wxime_poll_sequel(wxime_candidate *out, int max);

/* 诊断：listener 期间累计的枚举/门统计（复位数） */
int wxime_event_stats(int *enum_events, int *skipped_events);

/* listener 是否空闲（引擎会话线程不在我们回调里）——发键前等待用 */
int wxime_event_listener_idle(void);

#endif /* WXIME_EVENT_H */
