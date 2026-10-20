// wetype_translator.h —— 微信输入法引擎翻译器（librime Translator）
//
// Query(input, segment)：把当前拼音段喂给 wxhost（增量发键），取微信引擎
// 的候选返回给 librime。用户提交时通过 commit_notifier 发 S 选词，
// 引擎侧完成词频学习。
#ifndef WETYPE_TRANSLATOR_H_
#define WETYPE_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translation.h>
#include <rime/translator.h>

#include "wetype_client.h"

namespace wetype {

using namespace rime;

class WeTypeTranslator : public Translator {
 public:
  explicit WeTypeTranslator(const Ticket &ticket);
  ~WeTypeTranslator() override;

  an<Translation> Query(const string &input, const Segment &segment) override;

 private:
  // 提交回调：对照最近候选表发 S（引擎词频学习）
  void OnCommit(Context *ctx);

  WeTypeClient client_;
  string last_input_;                       // 上次已喂给引擎的键串
  vector<string> last_words_;               // 最近一次下发的候选（S 索引基准）
  connection commit_conn_;
  bool started_ = false;
};

}  // namespace wetype

#endif  // WETYPE_TRANSLATOR_H_
