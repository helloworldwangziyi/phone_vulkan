/// @file kv_store.cpp
/// KeyValueStore 的 MMKV 实现。MMKV.h 由构建系统把 third_party/mmkv/Core
/// 加进 include 路径后可见（头文件里只前向声明，使用方无需感知 MMKV）。
#include "evk/kv_store.h"

#include "evk/log.h"

#include "MMKV.h"

namespace evk {

KeyValueStore& KeyValueStore::instance() {
    static KeyValueStore store;
    return store;
}

void KeyValueStore::initialize(const std::string& rootDir) {
    KeyValueStore& self = instance();
    if (self.store_) {
        return; // 幂等：surface 重建等重复初始化直接跳过
    }
    if (rootDir.empty()) {
        EVK_LOGE("KeyValueStore initialize failed: empty rootDir");
        return;
    }
    // 日志级别压到 Warning：MMKV 的 Info 级日志在每次写盘时都会输出。
    MMKV::initializeMMKV(rootDir, MMKVLogWarning);
    self.store_ = MMKV::defaultMMKV();
    if (self.store_) {
        EVK_LOGI("KeyValueStore ready, root={}", rootDir);
    } else {
        EVK_LOGE("KeyValueStore initialize failed: defaultMMKV is null");
    }
}

bool KeyValueStore::isReady() const {
    return store_ != nullptr;
}

void KeyValueStore::warnNotReadyOnce() const {
    static bool warned = false;
    if (!warned) {
        warned = true;
        EVK_LOGW("KeyValueStore not initialized; get returns fallback, put is dropped. "
                 "Platform shell should call KeyValueStore::initialize() first.");
    }
}

bool KeyValueStore::putInt(const std::string& key, int32_t value) {
    if (!store_) {
        warnNotReadyOnce();
        return false;
    }
    return store_->set(value, key);
}

int32_t KeyValueStore::getInt(const std::string& key, int32_t fallback) const {
    if (!store_) {
        warnNotReadyOnce();
        return fallback;
    }
    return store_->getInt32(key, fallback);
}

bool KeyValueStore::putBool(const std::string& key, bool value) {
    if (!store_) {
        warnNotReadyOnce();
        return false;
    }
    return store_->set(value, key);
}

bool KeyValueStore::getBool(const std::string& key, bool fallback) const {
    if (!store_) {
        warnNotReadyOnce();
        return fallback;
    }
    return store_->getBool(key, fallback);
}

bool KeyValueStore::putString(const std::string& key, const std::string& value) {
    if (!store_) {
        warnNotReadyOnce();
        return false;
    }
    return store_->set(value, key);
}

std::string KeyValueStore::getString(const std::string& key, const std::string& fallback) const {
    if (!store_) {
        warnNotReadyOnce();
        return fallback;
    }
    std::string result;
    if (!store_->getString(key, result)) {
        return fallback;
    }
    return result;
}

bool KeyValueStore::remove(const std::string& key) {
    if (!store_) {
        warnNotReadyOnce();
        return false;
    }
    return store_->removeValueForKey(key);
}

bool KeyValueStore::contains(const std::string& key) const {
    if (!store_) {
        return false;
    }
    return store_->containsKey(key);
}

} // namespace evk
