#pragma once

// evk 日志封装，底层使用 third_party/spdlog（header-only）。
//
// 各平台壳启动时调用一次 evk::log::init(tag)：
// - Android：输出到 logcat；
// - 桌面：输出到 stdout；
// - iOS/HarmonyOS：后续接 os_log/hilog。
//
// 使用方式：
//   evk::log::init("estarx");
//   EVK_LOGI("renderer initialized, extent={}x{}", w, h);

#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#endif

namespace evk {
namespace log {

inline void init(const std::string& tag = "estarx") {
    auto logger = spdlog::get(tag);
    if (logger) {
        spdlog::set_default_logger(logger);
        return;
    }
#ifdef __ANDROID__
    logger = spdlog::android_logger_mt(tag, tag);
#else
    logger = spdlog::stdout_logger_mt(tag);
#endif
    logger->set_pattern("[%n] [%^%L%$] %v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
}

} // namespace log
} // namespace evk

#define EVK_LOGV(...) SPDLOG_TRACE(__VA_ARGS__)
#define EVK_LOGD(...) SPDLOG_DEBUG(__VA_ARGS__)
#define EVK_LOGI(...) SPDLOG_INFO(__VA_ARGS__)
#define EVK_LOGW(...) SPDLOG_WARN(__VA_ARGS__)
#define EVK_LOGE(...) SPDLOG_ERROR(__VA_ARGS__)
