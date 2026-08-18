#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/ui_runtime_test"
cxx=${CXX:-c++}
sdk_flags=

if [ "$(uname -s)" = "Darwin" ]; then
    cxx=$(xcrun --find clang++)
    sdk_flags="-isysroot $(xcrun --show-sdk-path)"
fi

"$cxx" $sdk_flags -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_root/core/include" \
    -I"$repo_root/third_party/spdlog/include" \
    -I"$repo_root/third_party/stb" \
    "$repo_root/tests/ui_runtime_test.cpp" \
    "$repo_root/core/src/app_lifecycle.cpp" \
    "$repo_root/core/src/frame_scheduler.cpp" \
    "$repo_root/core/src/ui/ui_application.cpp" \
    "$repo_root/core/src/ui/render_view.cpp" \
    "$repo_root/core/src/ui/pointer_input.cpp" \
    "$repo_root/core/src/ui/animation_scheduler.cpp" \
    "$repo_root/core/src/ui/paint_canvas.cpp" \
    "$repo_root/core/src/ui/event_bus.cpp" \
    "$repo_root/core/src/ui/font_engine.cpp" \
    "$repo_root/core/src/ui/texture_store.cpp" \
    "$repo_root/core/src/ui/view/button_control.cpp" \
    "$repo_root/core/src/ui/layout/flex_layout.cpp" \
    "$repo_root/core/src/ui/widget_tree.cpp" \
    "$repo_root/core/src/ui/controls/basic.cpp" \
    "$repo_root/core/src/ui/controls/button.cpp" \
    "$repo_root/core/src/ui/controls/container.cpp" \
    "$repo_root/core/src/ui/controls/flex.cpp" \
    "$repo_root/core/src/ui/controls/image.cpp" \
    "$repo_root/core/src/ui/controls/list_view.cpp" \
    "$repo_root/core/src/ui/controls/scroll_view.cpp" \
    "$repo_root/core/src/ui/controls/text.cpp" \
    "$repo_root/core/src/ui/view/scroll_control.cpp" \
    "$repo_root/core/src/ui/navigation/navigation_stack.cpp" \
    -o "$test_binary"

# 字体测试需要两个字体文件路径作为运行参数（argv[1]=拉丁、argv[2]=中文）。
"$test_binary" \
    "$repo_root/core/assets/fonts/roboto_regular_subset.ttf" \
    "$repo_root/core/assets/fonts/notosanssc_regular_subset.ttf"
