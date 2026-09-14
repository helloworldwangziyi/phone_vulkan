#pragma once

/**
 * @file platform_channel.h
 * @brief 引擎 ↔ 平台薄壳的双向"平台通道"（MethodChannel 式，按方法名路由）。
 *
 * 与 app_lifecycle.h 的封闭 EventId 枚举互补：平台通道走开放的方法名
 * 字符串，App 与平台壳各自按名注册/注入实现，新增互调不必改 core 的
 * 枚举定义。入向（平台→引擎）照 setEventFunc 的注册点惯例，出向
 * （引擎→平台）照 frame_scheduler.h 的 setFrameFunc 注入惯例。
 *
 * 设计约定（第一版）：
 * - 全部同步调用、UI 线程：与全项目单线程模型一致（对照 Android 侧
 *   NativeBridge.nativeOnBackPressed 的同步返回值语义），core 内部无锁；
 * - 负载为 UTF-8 字符串：结构化数据由 App 自行编码（如 JSON），
 *   core 不引 JSON 库；
 * - 未注册 handler 的方法调进 dispatchPlatformCall 返回空串；
 * - 未注入 invoker 时 invokePlatform 返回 false；
 * - 异步回调（callId 机制）留待 IME/相机等真实异步需求出现时扩展。
 */
#include <functional>
#include <string>

namespace evk {

/**
 * @brief 平台→引擎入向调用的处理器，App/core 按方法名注册。
 * @param args 平台侧带来的 UTF-8 参数串
 * @return 返回给平台的 UTF-8 结果串（空串表示无结果）
 */
using PlatformHandler = std::function<std::string(const std::string& args)>;

/**
 * @brief 注册命名处理器（平台→引擎入向）。重复注册同名方法时新 handler
 * 覆盖旧的；传空 handler 等价于 removePlatformHandler。
 * @param method 方法名（非空）
 * @param handler 处理器
 */
void setPlatformHandler(const char* method, PlatformHandler handler);

/**
 * @brief 摘除命名处理器；未注册过的方法名调用它是无害空操作。
 * @param method 方法名
 */
void removePlatformHandler(const char* method);

/**
 * @brief 三端桥层共用的入向出口：平台薄壳把方法名与参数串交给 core 路由。
 * @param method 方法名
 * @param args UTF-8 参数串（可为 nullptr，按空串处理）
 * @return 已注册 handler 的返回串；未注册时返回空串
 */
std::string dispatchPlatformCall(const char* method, const char* args);

/**
 * @brief 引擎→平台出向调用的实现，由三端桥层在各自 init 时注入
 * （setFrameFunc 同款模式）。
 * @param method 方法名
 * @param args UTF-8 参数串
 * @return 平台侧给出的 UTF-8 结果串
 */
using PlatformInvoker =
    std::function<std::string(const std::string& method, const std::string& args)>;

/**
 * @brief 注入出向实现；传 nullptr 表示注销（平台壳销毁时）。
 * @param invoker 出向实现
 */
void setPlatformInvoker(PlatformInvoker invoker);

/**
 * @brief 引擎/App 的出向调用入口。
 * @param method 方法名
 * @param args UTF-8 参数串
 * @param resultOut 结果串出口，可为 nullptr（不关心返回值时）
 * @return false 表示平台侧未注入 invoker（调用未发生）
 */
bool invokePlatform(const char* method, const std::string& args,
                    std::string* resultOut = nullptr);

} // namespace evk
