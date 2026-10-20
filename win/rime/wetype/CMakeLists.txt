project(rime-wetype)
cmake_minimum_required(VERSION 3.10)

# 微信输入法引擎翻译器：librime 插件，候选来自 wine 里的 wxhost.exe
# （win/wxhost，Route C 单 exe 直调 libwxhld.dll）。
# 集成方式：把本目录复制/链接到 librime 源码树 plugins/ 下，
# librime 的 CMake 会自动收入（见 win/rime/wetype/README.md）。

aux_source_directory(src wetype_src)

add_library(rime-wetype-objs OBJECT ${wetype_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-wetype-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-wetype PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-wetype-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "wetype" PARENT_SCOPE)
