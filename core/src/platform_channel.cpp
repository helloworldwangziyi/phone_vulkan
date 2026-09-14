/**
 * @file platform_channel.cpp
 * @brief 平台通道实现：方法名路由表（入向）与出向注入点的注册与转发。
 */
#include "evk/platform_channel.h"

#include <unordered_map>
#include <utility>

namespace {

// 入向路由表：方法名 → handler。函数内静态对象延迟构造，
// 与各平台壳的启动顺序无关（同 app_lifecycle.cpp 的 eventFunc 惯例）。
std::unordered_map<std::string, evk::PlatformHandler>& handlers() {
    static std::unordered_map<std::string, evk::PlatformHandler> table;
    return table;
}

// 出向注入点：桥层 init 时写入，UI 线程单写单读，无需锁。
evk::PlatformInvoker& platformInvoker() {
    static evk::PlatformInvoker func;
    return func;
}

} // namespace

namespace evk {

void setPlatformHandler(const char* method, PlatformHandler handler) {
    if (!method) {
        return;
    }
    if (handler) {
        handlers()[method] = std::move(handler);
    } else {
        handlers().erase(method);
    }
}

void removePlatformHandler(const char* method) {
    if (!method) {
        return;
    }
    handlers().erase(method);
}

std::string dispatchPlatformCall(const char* method, const char* args) {
    if (!method) {
        return {};
    }
    const auto it = handlers().find(method);
    if (it == handlers().end() || !it->second) {
        return {};
    }
    return it->second(args ? args : "");
}

void setPlatformInvoker(PlatformInvoker invoker) {
    platformInvoker() = std::move(invoker);
}

bool invokePlatform(const char* method, const std::string& args, std::string* resultOut) {
    PlatformInvoker& func = platformInvoker();
    if (!func || !method) {
        return false;
    }
    std::string result = func(method, args);
    if (resultOut) {
        *resultOut = std::move(result);
    }
    return true;
}

} // namespace evk
