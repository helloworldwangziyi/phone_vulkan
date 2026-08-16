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
    "$repo_root/tests/ui_runtime_test.cpp" \
    "$repo_root/core/src/app_lifecycle.cpp" \
    "$repo_root/core/src/frame_scheduler.cpp" \
    "$repo_root/core/src/ui/ui_application.cpp" \
    "$repo_root/core/src/ui/render_view.cpp" \
    "$repo_root/core/src/ui/pointer_input.cpp" \
    "$repo_root/core/src/ui/animation_scheduler.cpp" \
    "$repo_root/core/src/ui/paint_canvas.cpp" \
    "$repo_root/core/src/ui/event_bus.cpp" \
    "$repo_root/core/src/ui/controls/button_control.cpp" \
    "$repo_root/core/src/ui/layout/flex_layout.cpp" \
    "$repo_root/core/src/ui/widget_tree.cpp" \
    "$repo_root/core/src/ui/controls/scroll_control.cpp" \
    "$repo_root/core/src/ui/navigation/navigation_stack.cpp" \
    -o "$test_binary"

"$test_binary"
