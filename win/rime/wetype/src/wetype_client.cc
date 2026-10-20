// wetype_client.cc —— wxhost 行协议客户端实现
//
// 子进程拓扑：本插件（librime 进程内）→ wine wxhost.exe --serve（直调
// libwxhld.dll 的 Route C 宿主）。行协议与 fcitx5 插件一致，见
// win/wxhost/wxhost_serve.h。
#include "wetype_client.h"

#include <fcntl.h>

namespace wetype {

namespace {

//wine 前缀：优先环境变量（用户可能自管），缺省用引擎树内的 prefix
std::string WinePrefix(const std::string &engine_dir) {
  const char *env = getenv("WETYPE_WINEPREFIX");
  if (env && *env) return env;
  return engine_dir + "/prefix";
}

}  // namespace

bool WeTypeClient::Start(const std::string &host_path,
                         const std::string &engine_dir) {
  host_path_ = host_path;
  engine_dir_ = engine_dir;

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    fprintf(stderr, "[wetype] pipe 失败\n");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "[wetype] fork 失败\n");
    return false;
  }
  if (pid == 0) {
    // 子进程：wine wxhost.exe --serve <engine_dir>
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    // stderr 保持继承（wxhost 的 [wxhost]/[engine] 日志落到 rime 前端日志）
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    setenv("WINEPREFIX", WinePrefix(engine_dir).c_str(), 1);
    setenv("WINEDEBUG", "-all", 1);
    setenv("WINEDLLOVERRIDES", "mscoree,mshtml=", 1);
    execlp("wine", "wine", host_path.c_str(), "--serve", engine_dir.c_str(),
           (char *)nullptr);
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  child_pid_ = pid;
  in_ = fdopen(in_pipe[1], "w");
  out_ = fdopen(out_pipe[0], "r");
  if (!in_ || !out_) {
    fprintf(stderr, "[wetype] fdopen 失败\n");
    Stop();
    return false;
  }
  setvbuf(in_, nullptr, _IOLBF, 0);

  // 等 READY（引擎初始化 5s 词典等待 + 会话，宽限 40s）
  std::string line;
  for (int i = 0; i < 40; i++) {
    if (!ReadReply(&line, 1000)) continue;
    if (line == "READY") {
      fprintf(stderr, "[wetype] wxhost READY\n");
      ready_.store(true, std::memory_order_release);
      return true;
    }
  }
  fprintf(stderr, "[wetype] 等 READY 超时\n");
  Stop();
  return false;
}

void WeTypeClient::Stop() {
  if (starter_.joinable()) starter_.join();
  if (in_) { SendLine("Q"); fclose(in_); in_ = nullptr; }
  if (out_) { fclose(out_); out_ = nullptr; }
  if (child_pid_ > 0) {
    int status = 0;
    // 给 wine 优雅退出的机会，超时强杀
    for (int i = 0; i < 10; i++) {
      if (waitpid(child_pid_, &status, WNOHANG) != 0) { child_pid_ = -1; return; }
      usleep(200 * 1000);
    }
    kill(child_pid_, SIGKILL);
    waitpid(child_pid_, &status, 0);
    child_pid_ = -1;
  }
}

void WeTypeClient::EnsureStartedAsync() {
  if (IsAlive() || starting_.load()) return;
  // 进程僵死（已退出未收割）先同步清理（waitpid 非阻塞，微秒级）
  int status = 0;
  if (child_pid_ > 0 && waitpid(child_pid_, &status, WNOHANG) != 0) {
    child_pid_ = -1;
    if (in_) { fclose(in_); in_ = nullptr; }
    if (out_) { fclose(out_); out_ = nullptr; }
    ready_.store(false);
  }
  if (child_pid_ > 0) return;   // 活进程但未 READY：启动线程仍在等
  starting_.store(true);
  if (starter_.joinable()) starter_.join();
  starter_ = std::thread([this] {
    if (Start(host_path_, engine_dir_)) {
      fprintf(stderr, "[wetype] wxhost 后台启动成功\n");
    } else {
      fprintf(stderr, "[wetype] wxhost 后台启动失败，稍后重试\n");
    }
    starting_.store(false);
  });
}

bool WeTypeClient::SendLine(const std::string &line) {
  if (!in_ || fprintf(in_, "%s\n", line.c_str()) < 0) return false;
  return fflush(in_) == 0;
}

bool WeTypeClient::ReadReply(std::string *line, int timeout_ms) {
  // 先消费复位后残留的应答
  if (!pending_.empty()) {
    *line = pending_.front();
    pending_.erase(pending_.begin());
    return true;
  }
  if (!out_) return false;
  int fd = fileno(out_);
  // 只认协议行（READY/PONG/CAND/EMPTY/OK/ERR/UNCHANGED 开头），其余丢弃
  for (;;) {
    fd_set rfds;
    struct timeval tv { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    char buf[16384];
    if (!fgets(buf, sizeof buf, out_)) return false;
    size_t len = strlen(buf);
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
    if (len == 0) continue;
    bool protocol =
        !strncmp(buf, "READY", 5) || !strncmp(buf, "PONG", 4) ||
        !strncmp(buf, "CAND\t", 5) || !strncmp(buf, "CAND", 4) ||
        !strcmp(buf, "EMPTY") || !strcmp(buf, "OK") || !strcmp(buf, "ERR") ||
        !strcmp(buf, "UNCHANGED");
    if (!protocol) continue;   // 残留日志行，丢弃再等
    *line = buf;
    return true;
  }
}

bool WeTypeClient::Query(const std::string &keys,
                         std::vector<std::pair<std::string, std::string>> *out) {
  out->clear();
  EnsureStartedAsync();
  if (!IsAlive()) return false;   // 引擎启动中/失败：本轮快速放弃
  if (!SendLine("B " + keys)) return false;
  // 候选等待 = wxhost 内部静默窗 250ms + 引擎处理，宽限 3s
  std::string line;
  for (int i = 0; i < 30; i++) {
    if (!ReadReply(&line, 100)) continue;
    if (line.compare(0, 5, "CAND\t") == 0) {
      size_t pos = 5;
      while (pos < line.size()) {
        size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) tab = line.size();
        if (tab > pos) {
          std::string word = line.substr(pos, tab - pos);
          if (!word.empty()) out->emplace_back(word, std::string());
        }
        pos = tab + 1;
      }
      return true;
    }
    if (line == "EMPTY") return true;   // 无候选（合法结果）
    if (line == "ERR") return false;
    // 其他行（OK 等迟到应答）继续等
  }
  return false;
}

bool WeTypeClient::Reset() {
  EnsureStartedAsync();
  if (!IsAlive() || !SendLine("C")) return false;
  std::string line;
  return ReadReply(&line, 1000) && line == "OK";
}

bool WeTypeClient::Select(int idx) {
  EnsureStartedAsync();
  if (!IsAlive() || !SendLine("S " + std::to_string(idx))) return false;
  std::string line;
  // S 的应答：OK（整段）/ CAND（剩余）/ ERR；都算完成（学习为目的）
  return ReadReply(&line, 2000);
}

}  // namespace wetype
