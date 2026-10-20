/* wxhost_serve.h —— vtype 行协议服务模式（与 harness/jinterop.c 的 daemon
 * 契约、Route W wetype_win_host.c 的命令集一致——fcitx5 插件无需改动即可
 * 在 Route W / Route C 后端间切换）。
 *
 * 命令集：
 *   B <keys> → 逐键输入 + 等候选静默窗 → CAND\t词1\t词2... / EMPTY
 *   L <key>  → 同 B（只发首键）
 *   S <idx>  → 选快照候选 → OK / CAND(剩余组词) / ERR
 *   C        → 会话重置（销毁重建，Route W 同语义） → OK
 *   PING→PONG  SAVE→OK  Q→退出
 *   OPT spans/tokens → ERR（插件自动降级）  R → UNCHANGED  HK → ERR
 */
#ifndef WXHOST_SERVE_H
#define WXHOST_SERVE_H

#include "wxime_engine.h"

/* 加载 stdin 命令循环直到 Q 或 EOF。引擎须已 wxime_open。 */
int wxhost_serve(wxime_engine *eng);

#endif /* WXHOST_SERVE_H */
