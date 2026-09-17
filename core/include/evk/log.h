#pragma once

/**
 * @file log.h
 * @brief evk 日志封装，底层使用 third_party/spdlog（header-only）。
 *
 * core 只提供便携默认（stdout）；平台壳可自己构造 spdlog logger
 * （如 Android 的 logcat sink、iOS 的 os_log）后用 init(logger) 注入：
 * @code{.cpp}
 * evk::log::init("estarx");                            // stdout（桌面/测试/默认）
 * evk::log::init(spdlog::android_logger_mt(tag, tag)); // Android 壳注入 logcat
 * @endcode
 *
 * 所有项目日志统一输出：
 *   [evk][<feature>][<event> key=value ...]
 *
 * 使用方式：
 * @code{.cpp}
 * evk::log::init("estarx");
 * EVK_LOGI("renderer", "initialized width={} height={}", w, h);
 * @endcode
 */

#include <memory>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace evk {
namespace log {

/**
 * @brief 把平台壳构造好的 logger 设为默认 logger（统一格式与级别）。
 * @param logger 平台壳构造好的 spdlog logger；为空则不做任何事
 */
inline void init(const std::shared_ptr<spdlog::logger>& logger) {
    if (!logger) {
        return;
    }
    // sink/系统日志负责时间、级别、进程与 tag；消息正文严格从 [evk] 开始。
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
}

/**
 * @brief 便携默认：stdout。桌面、测试及未定制 sink 的平台直接使用；
 * 同名 logger 重复 init 只生效一次。
 * @param tag logger 名（重复调用时按它复用已有 logger）
 */
inline void init(const std::string& tag = "estarx") {
    auto logger = spdlog::get(tag);
    if (!logger) {
        logger = spdlog::stdout_logger_mt(tag);
    }
    init(logger);
}

/**
 * @brief 写一条结构化项目日志。
 *
 * feature 是稳定、可过滤的功能域；format 的第一个 token 是稳定事件名，
 * 后续动态字段使用 key=value。统一封装确保 C++ 日志不会漏掉三段式前缀。
 */
template <typename... Args>
inline void write(spdlog::level::level_enum level,
                  spdlog::string_view_t feature,
                  spdlog::format_string_t<Args...> format,
                  Args&&... args) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    const auto detail = spdlog::fmt_lib::format(
        format, std::forward<Args>(args)...);
    logger->log(level, "[evk][{}][{}]", feature, detail);
}

} // namespace log
} // namespace evk

/// 日志宏：第一个参数必须是功能域，后续参数遵循 spdlog 格式串。
#define EVK_LOGV(feature, ...) \
    ::evk::log::write(spdlog::level::trace, feature, __VA_ARGS__)
#define EVK_LOGD(feature, ...) \
    ::evk::log::write(spdlog::level::debug, feature, __VA_ARGS__)
#define EVK_LOGI(feature, ...) \
    ::evk::log::write(spdlog::level::info, feature, __VA_ARGS__)
#define EVK_LOGW(feature, ...) \
    ::evk::log::write(spdlog::level::warn, feature, __VA_ARGS__)
#define EVK_LOGE(feature, ...) \
    ::evk::log::write(spdlog::level::err, feature, __VA_ARGS__)
