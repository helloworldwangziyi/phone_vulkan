# 把字体二进制内嵌为 C++ 字节数组头文件（对照 embed_ui_shaders.cmake 的做法，
# 免运行时读文件、三端共用同一份资产）。
#
# 用法（script 模式）：
#   cmake -DFONT_LIST="kFontA=path/a.ttf;kFontB=path/b.ttf" \
#         -DOUTPUT_HEADER=out/evk/assets/font_assets.h \
#         -P embed_font_assets.cmake
#
# 生成内容形如：
#   namespace evk::assets {
#   inline constexpr unsigned char kFontA[] = { 0x00, 0x01, ... };
#   inline constexpr unsigned char kFontB[] = { ... };
#   }
# 字节数由 sizeof(数组) 获得，不再单独生成长度符号。

if(NOT DEFINED FONT_LIST)
    message(FATAL_ERROR "FONT_LIST is not set")
endif()
if(NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "OUTPUT_HEADER is not set")
endif()

get_filename_component(output_dir "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

file(WRITE "${OUTPUT_HEADER}" [==[
// 由 cmake/embed_font_assets.cmake 自动生成，勿手改。
// 字体来源与许可证见 core/assets/fonts/（SIL Open Font License 1.1）。
#pragma once

#include <cstddef>

namespace evk::assets {

]==])

foreach(entry IN LISTS FONT_LIST)
    string(FIND "${entry}" "=" eq_pos)
    if(eq_pos LESS 0)
        message(FATAL_ERROR "FONT_LIST entry '${entry}' must be NAME=PATH")
    endif()
    string(SUBSTRING "${entry}" 0 ${eq_pos} array_name)
    math(EXPR path_start "${eq_pos} + 1")
    string(LENGTH "${entry}" entry_len)
    math(EXPR path_len "${entry_len} - ${eq_pos} - 1")
    string(SUBSTRING "${entry}" ${path_start} ${path_len} font_path)

    if(NOT EXISTS "${font_path}")
        message(FATAL_ERROR "font file not found: ${font_path}")
    endif()

    file(READ "${font_path}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " formatted "${hex}")

    file(APPEND "${OUTPUT_HEADER}"
        "inline constexpr unsigned char ${array_name}[] = { ${formatted} };\n")

    message(STATUS "embedded font ${array_name} <- ${font_path}")
endforeach()

file(APPEND "${OUTPUT_HEADER}" [==[
} // namespace evk::assets
]==])
