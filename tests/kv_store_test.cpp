/// @file kv_store_test.cpp
/// KeyValueStore（MMKV 实现）的主机侧验证：
///   1. 未初始化时 get 返回 fallback、put 被丢弃（壳层漏接的保护行为）；
///   2. 同进程写入后立即读回（roundtrip）；
///   3. 跨进程持久化：write 子命令写盘退出，read 子命令重新 initialize
///      后必须读到原值——模拟「冷启动后设置仍在」的真实场景。
///
/// 运行：tests/run_kv_store_test.sh <workdir>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "evk/kv_store.h"

namespace {

int g_failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::printf("ok: %s\n", what);
    }
}

/// 三个类型的写入值（write 与 read 两边保持一致）。
constexpr const char* kKeyInt = "watchlist.bottomNav";
constexpr int32_t kValueInt = 2;
constexpr const char* kKeyBool = "settings.nightMode";
constexpr bool kValueBool = true;
constexpr const char* kKeyStr = "settings.userName";
const std::string kValueStr = "estarx";

void writeValues() {
    auto& kv = evk::KeyValueStore::instance();
    expect(kv.putInt(kKeyInt, kValueInt), "putInt returns true");
    expect(kv.putBool(kKeyBool, kValueBool), "putBool returns true");
    expect(kv.putString(kKeyStr, kValueStr), "putString returns true");
}

void checkValues(const char* phase) {
    auto& kv = evk::KeyValueStore::instance();
    std::string prefix = std::string(phase) + ": ";
    expect(kv.getInt(kKeyInt, -1) == kValueInt, (prefix + "getInt roundtrip").c_str());
    expect(kv.getBool(kKeyBool, false) == kValueBool, (prefix + "getBool roundtrip").c_str());
    expect(kv.getString(kKeyStr, "") == kValueStr, (prefix + "getString roundtrip").c_str());
    expect(kv.contains(kKeyInt), (prefix + "contains existing key").c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <write|read|roundtrip> <dir>\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const std::string dir = argv[2];

    // 未初始化保护：get 回 fallback，put 返回 false（只告警一次）。
    auto& kv = evk::KeyValueStore::instance();
    expect(!kv.isReady(), "not ready before initialize");
    expect(kv.getInt(kKeyInt, 7) == 7, "fallback when not ready");
    expect(!kv.putInt(kKeyInt, 1), "put dropped when not ready");

    evk::KeyValueStore::initialize(dir);
    expect(kv.isReady(), "ready after initialize");

    if (mode == "write") {
        writeValues();
        checkValues("write-process");
    } else if (mode == "read") {
        // 新进程冷启动：必须先读到上一个进程写下的值。
        checkValues("read-process");
        // remove 后 contains 为 false，get 回 fallback。
        expect(kv.remove(kKeyInt), "remove returns true");
        expect(!kv.contains(kKeyInt), "removed key not contained");
        expect(kv.getInt(kKeyInt, 9) == 9, "fallback after remove");
    } else if (mode == "roundtrip") {
        writeValues();
        checkValues("roundtrip");
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("kv_store_test(%s) passed\n", mode.c_str());
    return 0;
}
