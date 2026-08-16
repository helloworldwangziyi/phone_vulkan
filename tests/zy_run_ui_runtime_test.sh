#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/zy_ui_runtime_test"
cxx=${CXX:-c++}
sdk_flags=

if [ "$(uname -s)" = "Darwin" ]; then
    cxx=$(xcrun --find clang++)
    sdk_flags="-isysroot $(xcrun --show-sdk-path)"
fi

"$cxx" $sdk_flags -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_root/core/include" \
    -I"$repo_root/third_party/spdlog/include" \
    "$repo_root/tests/zy_ui_runtime_test.cpp" \
    "$repo_root/core/src/zy_app_lifecycle.cpp" \
    "$repo_root/core/src/zy_frame_scheduler.cpp" \
    "$repo_root/core/src/ui/zy_ui_application.cpp" \
    "$repo_root/core/src/ui/zy_render_view.cpp" \
    "$repo_root/core/src/ui/zy_pointer_input.cpp" \
    "$repo_root/core/src/ui/zy_animation_scheduler.cpp" \
    "$repo_root/core/src/ui/zy_paint_canvas.cpp" \
    "$repo_root/core/src/ui/zy_event_bus.cpp" \
    "$repo_root/core/src/ui/controls/zy_button_control.cpp" \
    "$repo_root/core/src/ui/layout/zy_flex_layout.cpp" \
    "$repo_root/core/src/ui/zy_widget_tree.cpp" \
    "$repo_root/core/src/ui/controls/zy_scroll_control.cpp" \
    "$repo_root/core/src/ui/navigation/zy_navigation_stack.cpp" \
    -o "$test_binary"

"$test_binary"
