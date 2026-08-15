/**
 * @file event.cpp
 * @brief 生命周期事件通道实现：唯一入口回调的注册与转发。
 */
#include "evk/event.h"

namespace {

/// 已注册的生命周期入口（setEventFunc 写入，dispatchEvent 读取）。
evk::EventFunc g_eventFunc = nullptr;

} // namespace

namespace evk {

void setEventFunc(EventFunc func) {
    g_eventFunc = func;
}

void dispatchEvent(EventId id, const void* data) {
    if (g_eventFunc) {
        g_eventFunc(id, data);
    }
}

} // namespace evk
