#pragma once

/**
 * @file zy_app_lifecycle.h
 * @brief 平台薄壳 → 公共 App 的生命周期事件通道。
 *
 * 平台薄壳在生命周期变化时调 dispatchEvent；公共 App 用 setEventFunc 注册
 * 唯一入口。View 输入不经过此通道。
 */
#include <cstdint>
#include <functional>

namespace evk {

/**
 * @brief 平台薄壳上报给公共 App 的生命周期事件。View 输入不经过此通道。
 */
enum class EventId : int32_t {
    EngineReady      = 1, ///< 渲染引擎初始化完成，可安全创建视图
    SurfaceChanged   = 2, ///< surface 尺寸/方向变化，data 为 SurfaceChangedData
    SurfaceDestroyed = 3, ///< surface 已销毁，渲染资源需释放
};

/**
 * @brief SurfaceChanged 事件携带的 surface 新尺寸。
 */
struct SurfaceChangedData {
    int32_t width;  ///< 新宽度
    int32_t height; ///< 新高度
};

/**
 * @brief 生命周期回调。View 输入和绘制由 ui 子系统直接分发，不进入这个全局通道。
 * @param id 事件号
 * @param data 事件附带数据：SurfaceChanged 时为 SurfaceChangedData*，其余为 nullptr
 */
using EventFunc = std::function<void(EventId id, const void* data)>;

/**
 * @brief 注册公共 App 生命周期入口，应在任一平台启动前完成。
 * @param func 生命周期回调
 */
void setEventFunc(EventFunc func);

/**
 * @brief 各平台薄壳共用的生命周期出口；未注册入口时静默丢弃。
 * @param id 事件号
 * @param data 事件附带数据（原样透传给已注册回调，可为 nullptr）
 */
void dispatchEvent(EventId id, const void* data);

} // namespace evk
