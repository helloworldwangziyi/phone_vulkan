/**
 * @file app_lifecycle.cpp
 * @brief 事件通道实现：唯一入口回调的注册与转发。
 */
#include "evk/app_lifecycle.h"

#include <utility>

namespace {

evk::EventFunc& eventFunc() {
    static evk::EventFunc func;
    return func;
}

} // namespace

namespace evk {

void setEventFunc(EventFunc func) {
    eventFunc() = std::move(func);
}

bool dispatchEvent(EventId id, const void* data) {
    EventFunc& func = eventFunc();
    return func ? func(id, data) : false;
}

} // namespace evk
