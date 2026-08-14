#pragma once

#include <cstdint>

namespace evk {

// 平台薄壳上报给公共 App 的生命周期事件。View 输入不经过此通道。
enum class EventId : int32_t {
    EngineReady      = 1,  // 渲染引擎初始化完成，可安全创建视图
    SurfaceChanged   = 2,
    SurfaceDestroyed = 3,
};

struct SurfaceChangedData {
    int32_t width;
    int32_t height;
};

// 生命周期回调。View 输入和绘制由 ui 子系统直接分发，不进入这个全局通道。
using EventFunc = void (*)(EventId id, const void* data);

// 注册公共 App 生命周期入口，应在任一平台启动前完成。
void setEventFunc(EventFunc func);

// 各平台薄壳共用的生命周期出口；未注册入口时静默丢弃。
void dispatchEvent(EventId id, const void* data);

} // namespace evk
