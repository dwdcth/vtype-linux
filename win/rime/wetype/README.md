# rime-wetype —— 微信输入法引擎的 librime 插件

让任何 rime 前端（fcitx5-rime / ibus-rime）直接用微信输入法的模型与词库：
候选来自真实引擎（wine 里的 wxhost.exe 直调 libwxhld.dll），不是词库转换。

## 原理

```
fcitx5-rime ── librime ── rime-wetype.so（本插件，Translator）
                                │  子进程管道（行协议）
                                ▼
                    wine wxhost.exe --serve（win/wxhost，Route C 宿主）
                                │  进程内直调
                                ▼
                    libwxhld.dll（微信输入法引擎 + 全部词典/NNLM）
```

- 逐键增量喂给引擎（前缀延长只发后缀），候选表实时返回
- 用户提交时回传 S 选词，引擎完成词频学习（与微信输入法本体共享用户词典）
- wxhost 崩溃/退出时自动重启

## 前置

1. 已安装 Windows 引擎与宿主（Route W/C 通用）：

   ```sh
   sh win/scripts/install_win_engine.sh    # → ~/.local/lib/wetype-ime/win64
   cp win/bin/wxhost.exe ~/.local/lib/wetype-ime/win64/
   ```

2. librime ≥ 1.7（插件机制）

## 编译安装（随 librime 一起）

```sh
git clone https://github.com/rime/librime.git
cd librime
# 插件源码就位（或用 install-plugins.sh 从本仓库拉取）
mkdir -p plugins && cp -r <本仓库>/win/rime/wetype plugins/
make deps
make
sudo make install        # 插件 .so 随 librime 一起装
```

或从 GitHub 一键装插件（需本仓库已推送）：

```sh
bash install-plugins.sh zhl-HFUT/vtype-linux@master:win/rime/wetype
```

## 启用（输入方案）

把 `wetype.schema.yaml` 复制到 Rime 用户目录，重新部署，从方案选单选“微信拼音”。

环境变量（可选）：

- `WETYPE_WIN_ENGINE_DIR`：引擎目录（缺省 `~/.local/lib/wetype-ime/win64`）
- `WETYPE_WINEPREFIX`：wine 前缀（缺省引擎目录下 `prefix/`）

## 已知边界

- 标点/英文由 rime 方案自身的 processor 处理，微信引擎只出中文候选
- 引擎候选动辄数百，插件取首屏 10 个（`kMaxCandidates` 可调）
- 首次按键有引擎启动等待（词典加载 ~10s），之后常驻
