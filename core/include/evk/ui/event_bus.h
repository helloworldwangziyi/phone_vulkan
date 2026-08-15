#pragma once

/**
 * @file event_bus.h
 * @brief esx 业务事件总线：带优先级的注册-派发（对齐 estarx App esx_common_event 模型）。
 *
 * 与 evk/event.h 的区别：event.h 是平台→App 的生命周期单入口；
 * 本文件是 App 内部的业务事件通道（行情到达、主题切换、自定义消息……）。
 *
 * 派发规则：
 * - 按优先级升序（HIGH 先派），同优先级按注册顺序；
 * - scope=0 为全局监听，始终被调用；
 * - scope 为视图时，仅当该视图及父链全部可见且挂在当前根视图树上才调用
 *   （页面被 Navigation 覆盖时其后代注册者收不到事件 = 隐式 pause）；
 * - 回调返回非 0 表示消费该事件，停止向后续注册者派发。
 *
 * @note 线程契约：esx_event_on/off/emit 只允许在 UI 线程调用；
 * esx_post_ui 是唯一的跨线程入口（后台数据源经它回 UI 线程）。
 */

#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 事件派发优先级（值越小越先派发）。
 */
typedef enum esx_event_priority {
    ESX_PRI_HIGH   = 0, ///< 最高优先级，最先派发
    ESX_PRI_NORMAL = 1, ///< 默认优先级
    ESX_PRI_LOW    = 2, ///< 最低优先级，最后派发
} esx_event_priority;

/**
 * @brief 事件回调。data 含义由 event_id 自定义（可为 NULL）。
 * @param event_id 事件号
 * @param data 事件数据
 * @param user_data 注册时传入的用户指针
 * @return 非 0 = 消费，停止派发
 */
typedef int32_t (*esx_event_func)(int32_t event_id, const void* data, void* user_data);

/**
 * @brief 注册事件监听。event_id 由 App 自定义（建议从 1 起）。同一 (event_id, func, user_data)
 * 组合可重复注册为不同 scope/优先级，off 时全部移除。
 * @param event_id 事件号
 * @param priority 派发优先级
 * @param scope 0=全局监听；否则仅该视图可见时派发
 * @param func 事件回调
 * @param user_data 透传给回调的用户指针
 */
void esx_event_on(int32_t event_id, esx_event_priority priority, esx_view scope,
                  esx_event_func func, void* user_data);

/**
 * @brief 注销事件监听：移除所有匹配 (event_id, func, user_data) 的注册。
 * @param event_id 事件号
 * @param func 事件回调
 * @param user_data 注册时传入的用户指针
 */
void esx_event_off(int32_t event_id, esx_event_func func, const void* user_data);

/**
 * @brief 派发事件：按 (priority, order) 顺序回调注册了 event_id 的监听者。
 * @param event_id 事件号
 * @param data 事件数据（原样透传给回调，可为 NULL）
 */
void esx_event_emit(int32_t event_id, const void* data);

/**
 * @brief 跨线程任务回调。
 * @param user_data 投递时传入的用户指针
 */
typedef void (*esx_task_func)(void* user_data);

/**
 * @brief 跨线程任务：任意线程调用，任务在下一个 UI 线程 beginFrame 开头执行。
 * user_data 的所有权由调用方规划（任务执行后自行释放）。
 * @param func 任务回调
 * @param user_data 透传给任务的用户指针
 */
void esx_post_ui(esx_task_func func, void* user_data);

#ifdef __cplusplus
} // extern "C"

#include <functional>

// ---- core 内部函数（非 ABI）----

/**
 * @brief 执行并清空待办 UI 任务队列；由 render_loop beginFrame 调用。
 */
void esxDrainUiTasks();

namespace evk::ui {

/**
 * @brief std::function 版跨线程投递（堆槽自动管理，任务执行后释放）。
 * @param fn 任务闭包
 */
void postUi(std::function<void()> fn);

} // namespace evk::ui
#endif
