// wetype_module.cc —— rime-wetype 插件入口：注册微信输入法引擎翻译器
//
// 形态：librime 插件（librime/plugins/wetype/）。加载后任何 rime 前端
// （fcitx5-rime / ibus-rime）都可用微信输入法引擎的模型与词库——引擎
// 本体在 wine 里跑（win/wxhost/wxhost.exe --serve 行协议宿主），
// 本插件通过子进程管道与它通信。
//
// 安装：随 librime 编译（install-plugins.sh zhl-HFUT/vtype-linux@master
// 取 win/rime/wetype），或参照 win/rime/wetype/README.md 手动编译。
#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "wetype_translator.h"

using namespace rime;

static void rime_wetype_initialize() {
  Registry &r = Registry::instance();
  r.Register("wetype_translator", new Component<wetype::WeTypeTranslator>);
}

static void rime_wetype_finalize() {
}

RIME_REGISTER_MODULE(wetype)
