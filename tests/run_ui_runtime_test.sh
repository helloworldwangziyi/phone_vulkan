#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/estarx_vulkan_ui_runtime_test"
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
    "$repo_root/core/src/render_loop.cpp" \
    "$repo_root/core/src/esx_view.cpp" \
    "$repo_root/core/src/ui/view.cpp" \
    "$repo_root/core/src/ui/input.cpp" \
    "$repo_root/core/src/ui/animator.cpp" \
    "$repo_root/core/src/ui/canvas.cpp" \
    "$repo_root/core/src/ui/controls/button.cpp" \
    "$repo_root/core/src/ui/controls/scroll_view.cpp" \
    "$repo_root/core/src/ui/controls/navigation.cpp" \
    -o "$test_binary"

"$test_binary"
