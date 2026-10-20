// wetype_client.h —— wxhost 行协议客户端（POSIX 子进程管道）
//
// 职责：spawn `wine wxhost.exe --serve`（win/wxhost 的 Route C 宿主），
// 用 stdin/stdout 行协议（与 fcitx5 插件契约一致）查询候选。
// 协议：B <keys> → CAND\t词1\t词2.../EMPTY；C → OK；S <idx> → OK/CAND/ERR。
#ifndef WETYPE_CLIENT_H_
#define WETYPE_CLIENT_H_

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace wetype {

class WeTypeClient {
 public:
  ~WeTypeClient() { Stop(); }

  // 启动 wxhost 子进程。host_path 为 wxhost.exe 路径（wine 程序），
  // engine_dir 为引擎目录（BuildIn/ 所在，传给 wxhost 做工作目录）。
  // 失败返回 false（原因已打印）。
  bool Start(const std::string &host_path, const std::string &engine_dir);

  // 首次发起后台启动（translator 惰性触发；内部走 EnsureStartedAsync）
  void KickoffStart(const std::string &host_path, const std::string &engine_dir) {
    host_path_ = host_path;
    engine_dir_ = engine_dir;
    EnsureStartedAsync();
  }

  void Stop();

  // 活性 = ready 且进程真实存活（waitpid WNOHANG 探测：kill -9 后
  // ready_/child_pid_ 仍是陈旧值——实测曾致 EnsureStartedAsync 永不
  // 触发，wxhost 死后无法自愈。==0 表示子进程仍在运行未退出）。
  bool IsAlive() const {
    if (child_pid_ <= 0 || !ready_.load(std::memory_order_acquire)) return false;
    return waitpid(child_pid_, nullptr, WNOHANG) == 0;
  }

  // 发键（增量）并等候选。返回候选词表（UTF-8 词 + 拼音注释），
  // 超时/无候选返回空。call_id 用于日志对账。
  bool Query(const std::string &keys, std::vector<std::pair<std::string, std::string>> *out);

  // 会话重置（换输入串/提交后调用）
  bool Reset();

  // 选词（学习）。成功返回 true。
  bool Select(int idx);

 private:
  bool SendLine(const std::string &line);
  // 带超时读一行（跳过非协议行）。协议行以 CAND/EMPTY/OK/ERR/READY/PONG 开头。
  bool ReadReply(std::string *line, int timeout_ms);
  // 非阻塞启动：后台线程做 fork+等 READY（引擎初始化 15-20s，同步等会
  // 卡死 rime 前端 UI 线程——wxhost 崩溃恢复实测暴露）。启动中/失败时
  // Query 快速返回 false（用户打字表现为短暂无候选，引擎就绪后自愈）。
  void EnsureStartedAsync();

  pid_t child_pid_ = -1;
  FILE *in_ = nullptr;    // 写向子进程 stdin
  FILE *out_ = nullptr;   // 读子进程 stdout
  std::string host_path_;
  std::string engine_dir_;
  std::vector<std::string> pending_;  // 复位后待消费的多行应答
  std::atomic<bool> ready_{false};    // READY 已收
  std::atomic<bool> starting_{false};
  std::thread starter_;               // 后台启动线程
};

}  // namespace wetype

#endif  // WETYPE_CLIENT_H_
