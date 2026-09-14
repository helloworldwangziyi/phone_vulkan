#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/platform_channel_test"
cxx=${CXX:-c++}
sdk_flags=

if [ "$(uname -s)" = "Darwin" ]; then
    cxx=$(xcrun --find clang++)
    sdk_flags="-isysroot $(xcrun --show-sdk-path)"
fi

# platform_channel 无第三方依赖：只编实现与测试自身即可。
"$cxx" $sdk_flags -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_root/core/include" \
    "$repo_root/tests/platform_channel_test.cpp" \
    "$repo_root/core/src/platform_channel.cpp" \
    -o "$test_binary"

"$test_binary"
