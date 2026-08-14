#pragma once

// esx 业务事件总线：带优先级的注册-派发（对齐 estarx App esx_common_event 模型）。
//
// 与 evk/event.h 的区别：event.h 是平台→App 的生命周期单入口；
// 本文件是 App 内部的业务事件通道（行情到达、主题切换、自定义消息……）。
//
// 派发规则：
//   - 按优先级升序（HIGH 先派），同优先级按注册顺序；
//   - scope=0 为全局监听，始终被调用；
//   - scope 为视图时，仅当该视图及父链全部可见且挂在当前根视图树上才调用
//     （页面被 Navigation 覆盖时其后代注册者收不到事件 = 隐式 pause）；
//   - 回调返回非 0 表示消费该事件，停止向后续注册者派发。
//
// 线程契约：esx_event_on/off/emit 只允许在 UI 线程调用；
// esx_post_ui 是唯一的跨线程入口（后台数据源经它回 UI 线程）。

#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum esx_event_priority {
    ESX_PRI_HIGH   = 0,
    ESX_PRI_NORMAL = 1,
    ESX_PRI_LOW    = 2,
} esx_event_priority;

// 返回非 0 = 消费，停止派发。data 含义由 event_id 自定义（可为 NULL）。
typedef int32_t (*esx_event_func)(int32_t event_id, const void* data, void* user_data);

// event_id 由 App 自定义（建议从 1 起）。同一 (event_id, func, user_data)
// 组合可重复注册为不同 scope/优先级，off 时全部移除。
void esx_event_on(int32_t event_id, esx_event_priority priority, esx_view scope,
                  esx_event_func func, void* user_data);
void esx_event_off(int32_t event_id, esx_event_func func, const void* user_data);
void esx_event_emit(int32_t event_id, const void* data);

// 跨线程任务：任意线程调用，任务在下一个 UI 线程 beginFrame 开头执行。
// user_data 的所有权由调用方规划（任务执行后自行释放）。
typedef void (*esx_task_func)(void* user_data);
void esx_post_ui(esx_task_func func, void* user_data);

#ifdef __cplusplus
} // extern "C"

#include <functional>

// ---- core 内部函数（非 ABI）----
// 执行并清空待办 UI 任务队列；由 render_loop beginFrame 调用。
void esxDrainUiTasks();

namespace evk::ui {
// std::function 版跨线程投递（堆槽自动管理，任务执行后释放）。
void postUi(std::function<void()> fn);
} // namespace evk::ui
#endif
