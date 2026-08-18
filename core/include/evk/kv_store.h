#pragma once

/**
 * @file kv_store.h
 * @brief 键值持久化存储：App 设置项（开关、选中态、用户偏好）的落盘读写。
 *
 * 对照 Flutter 的 shared_preferences / 跨端的 MMKV：同步读写、mmap 落盘，
 * 单次操作微秒级，可以直接在 UI 线程调用（点按回调里随写随存）。
 *
 * 分工：core 只定义能力，不碰文件系统路径——存储根目录由平台薄壳在
 * 引擎启动最早时刻注入（Android 传 filesDir，iOS 传 Documents 目录），
 * 见 KeyValueStore::initialize()。未初始化时所有 get 返回 fallback，
 * put 丢弃并告警一次，保证壳层漏接时行为可预期而不是崩溃。
 *
 * 实现基于腾讯 MMKV（third_party/mmkv/Core），同步 API、跨进程安全。
 * 鸿蒙端 MMKV 官方已支持（>= 1.3.5），壳层接入时同样调 initialize 即可。
 *
 * 单线程模型：与引擎其它模块一致，全部接口只在 UI 线程调用。
 */
#include <cstdint>
#include <string>

// MMKV 主类位于全局命名空间（third_party/mmkv/Core/MMKV.h）。
class MMKV;

namespace evk {

/**
 * @brief 键值存储单例（默认库，全 App 共享一份）。
 *
 * 用法：
 * @code
 *   // 平台壳启动时（nativeInit / evkIosInit）：
 *   KeyValueStore::initialize(filesDir);
 *   // 业务侧：
 *   KeyValueStore::instance().putInt("watchlist.bottomNav", 2);
 *   int nav = KeyValueStore::instance().getInt("watchlist.bottomNav", 0);
 * @endcode
 *
 * key 约定「模块.设置名」小驼峰，避免不同页面互相覆盖。
 */
class KeyValueStore {
public:
    static KeyValueStore& instance();

    /**
     * @brief 注入存储根目录并完成初始化（幂等，重复调用只生效第一次）。
     *
     * 必须在 UI 线程、任何业务读写之前调用——由平台薄壳在引擎初始化
     * 最早时刻完成。目录不存在时会自动创建；数据落在 <rootDir>/mmkv/ 下。
     * @param rootDir 平台私有目录（Android filesDir / iOS Documents）
     */
    static void initialize(const std::string& rootDir);

    /// 是否已就绪（initialize 成功）。未就绪时 get 一律返回 fallback。
    bool isReady() const;

    /// 写入 int32；返回是否落盘成功（未就绪返回 false）。
    bool putInt(const std::string& key, int32_t value);
    /// 读取 int32；键不存在或未就绪时返回 fallback。
    int32_t getInt(const std::string& key, int32_t fallback) const;

    /// 写入 bool；返回是否落盘成功。
    bool putBool(const std::string& key, bool value);
    /// 读取 bool；键不存在或未就绪时返回 fallback。
    bool getBool(const std::string& key, bool fallback) const;

    /// 写入字符串；返回是否落盘成功。
    bool putString(const std::string& key, const std::string& value);
    /// 读取字符串；键不存在或未就绪时返回 fallback。
    std::string getString(const std::string& key, const std::string& fallback) const;

    /// 删除单个键；返回是否成功。
    bool remove(const std::string& key);

    /// 键是否存在（未就绪恒为 false）。
    bool contains(const std::string& key) const;

private:
    KeyValueStore() = default;

    /// 未就绪告警只打一次，避免每帧刷屏。
    void warnNotReadyOnce() const;

    MMKV* store_ = nullptr; ///< MMKV 默认库句柄（initialize 后非空）
};

} // namespace evk
