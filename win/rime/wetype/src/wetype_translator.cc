// wetype_translator.cc —— 微信输入法引擎翻译器实现
//
// 数据流：librime 每键 Query(拼音段) → 增量喂 wxhost（B 命令一次调用
// 同时喂键并返回当前完整候选表）→ FifoTranslation 返回给 librime。
// 提交时 commit_notifier → 对照候选表发 S（引擎词频学习；wxhost 的 S
// 整段完成语义会自动重置会话）。
#include <unistd.h>

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/segmentation.h>

#include "wetype_translator.h"

namespace wetype {

namespace {

// wxhost 与引擎目录发现：显式环境变量优先，缺省用标准安装位
// （install_win_engine.sh 装的 ~/.local/lib/wetype-ime/win64）
void FindHostPaths(std::string *host, std::string *engine_dir) {
  const char *dir = getenv("WETYPE_WIN_ENGINE_DIR");
  if (dir && *dir) {
    *engine_dir = dir;
    *host = std::string(dir) + "/wxhost.exe";
    return;
  }
  const char *home = getenv("HOME");
  if (!home) return;
  *engine_dir = std::string(home) + "/.local/lib/wetype-ime/win64";
  *host = *engine_dir + "/wxhost.exe";
}

// 下发给 librime 的候选上限（微信引擎动辄几百候选，首屏足够）
constexpr size_t kMaxCandidates = 10;

}  // namespace

WeTypeTranslator::WeTypeTranslator(const Ticket &ticket)
    : Translator(ticket) {
  if (engine_) {
    // 提交回调：把用户选中的词回传引擎学习（S 命令）
    commit_conn_ = engine_->context()->commit_notifier().connect(
        [this](Context *ctx) { OnCommit(ctx); });
  }
}

WeTypeTranslator::~WeTypeTranslator() {
  commit_conn_.disconnect();
}

an<Translation> WeTypeTranslator::Query(const string &input,
                                        const Segment &segment) {
  if (!segment.HasTag("abc"))
    return nullptr;

  // 惰性启动（wxhost 初始化含 5s 词典等待，不能阻塞会话创建）
  if (!started_) {
    std::string host, engine_dir;
    FindHostPaths(&host, &engine_dir);
    if (host.empty() || access(host.c_str(), R_OK) != 0) {
      LOG(INFO) << "wetype: 未找到 wxhost.exe（设 WETYPE_WIN_ENGINE_DIR），本会话跳过";
      return nullptr;
    }
    if (!client_.Start(host, engine_dir)) {
      LOG(WARNING) << "wetype: wxhost 启动失败";
      return nullptr;
    }
    started_ = true;
  }

  // 增量喂键：输入串在上次基础上延长 → 只发后缀；否则重置会话全量重打。
  // B 命令一次调用同时喂键并返回当前完整候选表。
  std::vector<std::pair<std::string, std::string>> words;
  bool ok = false;
  if (!last_input_.empty() &&
      input.size() > last_input_.size() &&
      input.compare(0, last_input_.size(), last_input_) == 0) {
    ok = client_.Query(input.substr(last_input_.size()), &words);
  } else {
    if (!last_input_.empty()) client_.Reset();
    ok = client_.Query(input, &words);
  }
  if (!ok) {
    fprintf(stderr, "[wetype] Query(\"%s\") 喂键失败\n", input.c_str());
    last_input_.clear();
    return nullptr;
  }
  last_input_ = input;
  last_words_.clear();

  auto translation = New<FifoTranslation>();
  for (size_t i = 0; i < words.size() && i < kMaxCandidates; i++) {
    auto cand = New<SimpleCandidate>("wetype", segment.start, segment.end,
                                     words[i].first, words[i].second);
    translation->Append(cand);
    last_words_.push_back(words[i].first);
  }
  if (translation->exhausted())
    return nullptr;
  return translation;
}

void WeTypeTranslator::OnCommit(Context *ctx) {
  if (!ctx) return;
  string text = ctx->GetCommitText();
  if (text.empty()) return;
  // 对照最近下发的候选表找索引，发 S 学习
  for (size_t i = 0; i < last_words_.size(); i++) {
    if (last_words_[i] == text) {
      client_.Select((int)i);
      break;
    }
  }
  // wxhost 的 S 整段完成语义会自动重置引擎会话；本侧同步清状态
  last_input_.clear();
  last_words_.clear();
}

}  // namespace wetype
