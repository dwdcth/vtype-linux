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
#include <string>
#include <vector>

namespace wetype {

class WeTypeClient {
 public:
  ~WeTypeClient() { Stop(); }

  // 启动 wxhost 子进程。host_path 为 wxhost.exe 路径（wine 程序），
  // engine_dir 为引擎目录（BuildIn/ 所在，传给 wxhost 做工作目录）。
  // 失败返回 false（原因已打印）。
  bool Start(const std::string &host_path, const std::string &engine_dir);

  void Stop();

  bool IsAlive() const { return child_pid_ > 0; }

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
  bool EnsureStarted();

  pid_t child_pid_ = -1;
  FILE *in_ = nullptr;    // 写向子进程 stdin
  FILE *out_ = nullptr;   // 读子进程 stdout
  std::string host_path_;
  std::string engine_dir_;
  std::vector<std::string> pending_;  // 复位后待消费的多行应答
};

}  // namespace wetype

#endif  // WETYPE_CLIENT_H_
