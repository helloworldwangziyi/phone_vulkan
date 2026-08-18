#!/bin/sh
# 主机侧 KeyValueStore 测试：编译 core 封装 + MMKV Core（POSIX 路径，
# FORCE_POSIX + MMKV_EMBED_ZLIB=1，与 iOS 手编配置一致），跑三轮：
#   roundtrip 同进程写读 → write 写盘退出 → read 冷启动读回。
#
# MMKV Core 实现需要 C++20（其 CMakeLists 同样要求），而消费方代码
# （kv_store.cpp / 测试）保持项目统一的 C++17——分两段编译正是为了
# 验证 MMKV.h 公共头可以被 C++17 的调用方 include（Android/iOS 同此组合）。
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/kv_store_test"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/kv_store_test.XXXXXX")
obj_dir=$(mktemp -d "${TMPDIR:-/tmp}/kv_store_obj.XXXXXX")
cxx=${CXX:-c++}
sdk_flags=

if [ "$(uname -s)" = "Darwin" ]; then
    cxx=$(xcrun --find clang++)
    sdk_flags="-isysroot $(xcrun --show-sdk-path)"
fi

mmkv="$repo_root/third_party/mmkv/Core"

# MMKV 手编源清单：跨平台公共文件。平台特定文件（*_Android/_OSX/_Win32/
# _Linux、crc32_armv8、.S 汇编）在 FORCE_POSIX 下为空或不适用，不编。
mmkv_sources="
MMKV.cpp
MMKV_IO.cpp
MMKVLog.cpp
CodedInputData.cpp
CodedInputDataCrypt.cpp
CodedOutputData.cpp
KeyValueHolder.cpp
PBUtility.cpp
MiniPBCoder.cpp
MMBuffer.cpp
InterProcessLock.cpp
MemoryFile.cpp
ThreadLock.cpp
aes/AESCrypt.cpp
aes/openssl/openssl_aes_core.cpp
aes/openssl/openssl_cfb128.cpp
aes/openssl/openssl_md5_dgst.cpp
aes/openssl/openssl_md5_one.cpp
crc32/zlib/crc32.cpp
"

# 第一段：MMKV 源以 C++20 编成 .o（第三方警告随 -isystem 与 -w 静默）。
objects=""
for src in $mmkv_sources; do
    obj="$obj_dir/$(echo "$src" | tr '/' '_').o"
    "$cxx" $sdk_flags -std=c++20 -w \
        -DFORCE_POSIX -DMMKV_EMBED_ZLIB=1 \
        -isystem "$mmkv" \
        -c "$mmkv/$src" -o "$obj"
    objects="$objects $obj"
done

# arm64 上 AES 接口宏映射到 .S 汇编符号（openssl_aes_arm_*），必须编入；
# x86_64 走纯 C 路径，跳过。
if [ "$(uname -m)" = "arm64" ]; then
    "$cxx" $sdk_flags -w -DFORCE_POSIX -DMMKV_EMBED_ZLIB=1 \
        -isystem "$mmkv" -isystem "$mmkv/aes/openssl" \
        -c "$mmkv/aes/openssl/openssl_aesv8-armx.S" -o "$obj_dir/aesv8_asm.o"
    objects="$objects $obj_dir/aesv8_asm.o"
fi

# 第二段：core 封装与测试以项目统一的 C++17 编译并链接。
"$cxx" $sdk_flags -std=c++17 -Wall -Wextra \
    -DFORCE_POSIX -DMMKV_EMBED_ZLIB=1 \
    -I"$repo_root/core/include" \
    -I"$repo_root/third_party/spdlog/include" \
    -isystem "$mmkv" \
    "$repo_root/tests/kv_store_test.cpp" \
    "$repo_root/core/src/kv_store.cpp" \
    $objects \
    -o "$test_binary"

"$test_binary" roundtrip "$work_dir"
"$test_binary" write "$work_dir"
"$test_binary" read "$work_dir"

rm -rf "$work_dir" "$obj_dir"
echo "kv_store_test: all passed"
