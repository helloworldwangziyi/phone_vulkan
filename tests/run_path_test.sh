#!/bin/sh
# 主机侧 Path 测试：只依赖 core/include 下的 path.h + path.cpp，
# 单文件编译即跑（细分/三角化纯 CPU 几何，不需要 Vulkan）。
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/path_test"
cxx=${CXX:-c++}
sdk_flags=

if [ "$(uname -s)" = "Darwin" ]; then
    cxx=$(xcrun --find clang++)
    sdk_flags="-isysroot $(xcrun --show-sdk-path)"
fi

"$cxx" $sdk_flags -std=c++17 -Wall -Wextra \
    -I"$repo_root/core/include" \
    "$repo_root/tests/path_test.cpp" \
    "$repo_root/core/src/ui/path.cpp" \
    -o "$test_binary"

"$test_binary"
