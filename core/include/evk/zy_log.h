#pragma once

/**
 * @file zy_log.h
 * @brief evk 日志封装，底层使用 third_party/spdlog（header-only）。
 *
 * core 只提供便携默认（stdout）；平台壳可自己构造 spdlog logger
 * （如 Android 的 logcat sink、iOS 的 os_log）后用 init(logger) 注入：
 * @code{.cpp}
 * evk::log::init("estarx");                            // stdout（桌面/测试/默认）
 * evk::log::init(spdlog::android_logger_mt(tag, tag)); // Android 壳注入 logcat
 * @endcode
 *
 * 使用方式：
 * @code{.cpp}
 * evk::log::init("estarx");
 * EVK_LOGI("renderer initialized, extent={}x{}", w, h);
 * @endcode
 */

#include <memory>
#include <string>

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
    logger->set_pattern("[%n] [%^%L%$] %v");
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

} // namespace log
} // namespace evk

/// 日志宏：V/D/I/W/E 五级，透传到 SPDLOG_*（默认 logger 即上面 init 的那个）。
#define EVK_LOGV(...) SPDLOG_TRACE(__VA_ARGS__)
#define EVK_LOGD(...) SPDLOG_DEBUG(__VA_ARGS__)
#define EVK_LOGI(...) SPDLOG_INFO(__VA_ARGS__)
#define EVK_LOGW(...) SPDLOG_WARN(__VA_ARGS__)
#define EVK_LOGE(...) SPDLOG_ERROR(__VA_ARGS__)
