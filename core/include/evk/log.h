#pragma once

// evk 日志封装，底层使用 third_party/spdlog（header-only）。
//
// core 只提供便携默认（stdout）；平台壳可自己构造 spdlog logger
// （如 Android 的 logcat sink、iOS 的 os_log）后用 init(logger) 注入：
//   evk::log::init("estarx");                          // stdout（桌面/测试/默认）
//   evk::log::init(spdlog::android_logger_mt(tag, tag)); // Android 壳注入 logcat
//
// 使用方式：
//   evk::log::init("estarx");
//   EVK_LOGI("renderer initialized, extent={}x{}", w, h);

#include <memory>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace evk {
namespace log {

// 把平台壳构造好的 logger 设为默认 logger（统一格式与级别）。
inline void init(const std::shared_ptr<spdlog::logger>& logger) {
    if (!logger) {
        return;
    }
    logger->set_pattern("[%n] [%^%L%$] %v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
}

// 便携默认：stdout。桌面、测试及未定制 sink 的平台直接使用；
// 同名 logger 重复 init 只生效一次。
inline void init(const std::string& tag = "estarx") {
    auto logger = spdlog::get(tag);
    if (!logger) {
        logger = spdlog::stdout_logger_mt(tag);
    }
    init(logger);
}

} // namespace log
} // namespace evk

#define EVK_LOGV(...) SPDLOG_TRACE(__VA_ARGS__)
#define EVK_LOGD(...) SPDLOG_DEBUG(__VA_ARGS__)
#define EVK_LOGI(...) SPDLOG_INFO(__VA_ARGS__)
#define EVK_LOGW(...) SPDLOG_WARN(__VA_ARGS__)
#define EVK_LOGE(...) SPDLOG_ERROR(__VA_ARGS__)
