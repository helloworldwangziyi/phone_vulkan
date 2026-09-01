// ============================================================================
// NAPI 实现层：ArkTS 薄壳（VulkanShell.ets 的 XComponent）与 core 之间的翻译层。
// 与 Android 的 android_jni_bridge.cpp 同构，本文件即 NAPI 书写规则的范例：
//
// 1. 模块注册：napi_module 结构体 + __attribute__((constructor)) 里调
//    napi_module_register。so 加载时构造函数先跑，模块进入进程全局注册表；
//    nm_modname 必须等于 ArkTS 侧 XComponent 的 libraryname（都是
//    "estarx_vulkan"，对应 libestarx_vulkan.so），框架按名字找回本模块。
//
// 2. Init 双职责：① napi_define_properties 把 C 函数挂上 exports（ArkTS 经
//    XComponent 的 onLoad(context) 拿到这个 exports 调用，context 用法等价于
//    import 加载模块的返回值）；② 从 exports 解包 OH_NATIVE_XCOMPONENT_OBJ__
//    得到 OH_NativeXComponent*，注册 XComponent 回调。框架只在 XComponent
//    触发的加载里挂这个对象，故解包失败不致命——函数照常可用。
//
// 3. 参数解包：napi_get_cb_info 取 argv；字符串用 napi_get_value_string_utf8
//    两段式（先问长度再读出）；整型时间戳用 napi_get_value_int64。
//
// 4. 线程规则：本文件所有函数都运行在 UI 线程——XComponent 四个回调、ArkTS
//    同步 NAPI 调用、DisplaySync 帧回调都在 UI 线程触发，与 Android 壳
//    （Choreographer + MotionEvent 同线程）模型一致，core 的无锁单线程假设
//    原样成立。这正是帧节奏选 ArkTS DisplaySync 而非 OH_NativeVSync 的原因：
//    后者回调在独立 vsync 线程，会迫使 core 引入锁。
//
// 设计约定：本层只做"解包参数 → 转发 core"，不写任何业务逻辑；
// 事件经 evk::dispatchEvent 回流给 App 注册的 EventFunc（见 app_lifecycle.h）。
// ============================================================================

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>

#include <cstdint>
#include <string>

#include <spdlog/sinks/callback_sink.h>

#include "evk/app_lifecycle.h"
#include "evk/kv_store.h"
#include "evk/log.h"
#include "evk/frame_scheduler.h"
#include "evk/compositor.h"
// 完整类型：g_platform->getSurfaceSize 与 compositor->renderer()->setSize 要用。
#include "evk/render_platform.h"
#include "evk/vulkan_renderer.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/pointer_input.h"

// hongmeng_vulkan_platform.cpp 以 C 链接导出的工厂函数（与 Android 同款约定）：
// bridge 只需声明原型即可调用，无需知道 HongmengPlatform 类的布局。
extern "C" evk::IPlatform* evkCreateHongmengPlatform(void* window, int32_t width, int32_t height);
extern "C" void evkDestroyHongmengPlatform(evk::IPlatform* platform);

// 全局 Compositor（帧编排器，内部持有渲染器）与平台适配器，
// 生命周期与 XComponent surface 一致（OnSurfaceCreated 建 / OnSurfaceDestroyed 拆）。
static evk::Compositor* g_compositor = nullptr;
static evk::IPlatform* g_platform = nullptr;
static bool g_appStarted = false;

// 单指跟踪（与 Java 壳 VulkanSurfaceView 的 activePointerId 同策略）：
// core 的指针输入是单指模型，多点触摸只跟随第一根手指。
static constexpr int32_t kNoPointer = -1;
static int32_t g_activePointerId = kNoPointer;

// hilog sink 由平台壳注入；core 默认只提供 stdout（见 evk/log.h）。
// spdlog 没有内置 hilog sink，用 callback_sink 转 OH_LOG_Print。
static void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::callback_logger_mt(
            "estarx-vulkan", [](const spdlog::details::log_msg& msg) {
                LogLevel level;
                switch (msg.level) {
                    case spdlog::level::trace:
                    case spdlog::level::debug: level = LOG_DEBUG; break;
                    case spdlog::level::info:  level = LOG_INFO;  break;
                    case spdlog::level::warn:  level = LOG_WARN;  break;
                    case spdlog::level::err:   level = LOG_ERROR; break;
                    default:                   level = LOG_FATAL; break;
                }
                // payload 不以 '\0' 结尾，拷贝成 C 字符串再交 hilog 的 %{public}s。
                const std::string text(msg.payload.data(), msg.payload.size());
                OH_LOG_Print(LOG_APP, level, 0x0000, "estarx-vulkan", "%{public}s", text.c_str());
            });
    }
    evk::log::init(logger);
}

// ---------------------------------------------------------------------------
// XComponent 回调（框架在 UI 线程触发）
// ---------------------------------------------------------------------------

// surface 创建就绪时调用（可能多次：页面重建）。幂等：已有渲染器直接返回。
static void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    ensureLogger();
    if (g_compositor) {
        return;
    }

    // XComponent 的 surface 尺寸（物理像素），建平台适配器与上报初始尺寸都要用。
    uint64_t surfaceWidth = 0;
    uint64_t surfaceHeight = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &surfaceWidth, &surfaceHeight) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        EVK_LOGE("OH_NativeXComponent_GetXComponentSize failed");
        return;
    }

    g_platform = evkCreateHongmengPlatform(window, static_cast<int32_t>(surfaceWidth),
                                           static_cast<int32_t>(surfaceHeight));
    if (!g_platform) {
        EVK_LOGE("failed to create Hongmeng platform");
        return;
    }

    g_compositor = new evk::Compositor(g_platform);
    if (!g_compositor->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 OnSurfaceCreated 能干净重试。
        EVK_LOGE("failed to initialize Vulkan renderer");
        delete g_compositor;
        g_compositor = nullptr;
        evkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    EVK_LOGI("Vulkan renderer initialized");
    // 平台壳的"画一帧"实现注册给 core，App 的 requestRender() 由此触发；
    // 帧编排（buildFrame + 首帧日志 + render）内聚在 evk::Compositor。
    evk::setFrameFunc([](int64_t) { if (g_compositor) g_compositor->renderFrame(); });
    // 渲染器初始化完成：core 进入就绪状态，之后 App 才能安全创建视图。
    evk::setEngineReady(true);
    // EngineReady 之前先报一次 SurfaceChanged：initialize() 成功后 surface 尺寸已知，
    // App 建视图树时即可按真实像素布局；之后的尺寸变化由 OnSurfaceChanged 上报。
    evk::SurfaceChangedData initSize{static_cast<int32_t>(surfaceWidth),
                                     static_cast<int32_t>(surfaceHeight)};
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &initSize);
    if (!g_appStarted) {
        g_appStarted = true;
        evk::dispatchEvent(evk::EventId::EngineReady, nullptr);
    }
    evk::requestRender();
}

// surface 尺寸变化（旋转、分屏等）。与 Android nativeResize 同语义。
static void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    evk::SurfaceChangedData data{static_cast<int32_t>(width), static_cast<int32_t>(height)};
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &data);
    if (g_compositor) {
        g_compositor->renderer()->setSize(static_cast<uint32_t>(width),
                                          static_cast<uint32_t>(height));
    }
    evk::requestRender();
}

// surface 销毁：与 Android nativeDestroy 同套清理，顺序也一致。
static void OnSurfaceDestroyedCB(OH_NativeXComponent* /*component*/, void* /*window*/) {
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    evk::cancelPendingFrame();
    evk::setFrameFunc(nullptr);
    evk::setEngineReady(false);
    g_appStarted = false; // 下次 OnSurfaceCreated 走完整 EngineReady 重建流程
    g_activePointerId = kNoPointer;
    if (g_compositor) {
        delete g_compositor;
        g_compositor = nullptr;
    }
    if (g_platform) {
        evkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("Vulkan renderer destroyed");
}

// 本层只把 XComponent 触摸类型翻译成跨平台 PointerAction
// （与 Android nativeOnTouch 的 switch 同款；两边枚举取值顺序不同，不能强转）。
static evk::ui::PointerAction toPointerAction(OH_NativeXComponent_TouchEventType type) {
    switch (type) {
        case OH_NATIVEXCOMPONENT_DOWN:   return evk::ui::PointerAction::Down;
        case OH_NATIVEXCOMPONENT_UP:     return evk::ui::PointerAction::Up;
        case OH_NATIVEXCOMPONENT_MOVE:   return evk::ui::PointerAction::Move;
        case OH_NATIVEXCOMPONENT_CANCEL: return evk::ui::PointerAction::Cancel;
        default:                         return evk::ui::PointerAction::Cancel;
    }
}

// 触摸事件。timeStamp 是系统启动以来的纳秒数，与 core 计算滑动速度所需的
// 时间基准一致，无需换算（Android 侧是毫秒 ×1e6 换算到纳秒）。
static void DispatchTouchEventCB(OH_NativeXComponent* component, void* window) {
    OH_NativeXComponent_TouchEvent touchEvent{};
    if (OH_NativeXComponent_GetTouchEvent(component, window, &touchEvent) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }

    switch (touchEvent.type) {
        case OH_NATIVEXCOMPONENT_DOWN:
            g_activePointerId = touchEvent.id;
            break;
        case OH_NATIVEXCOMPONENT_MOVE: {
            if (g_activePointerId == kNoPointer) {
                return;
            }
            // MOVE 批量上报所有触点，只转发被跟踪手指的坐标
            // （对应 Android 的 event.findPointerIndex(activePointerId)）。
            for (uint32_t i = 0; i < touchEvent.numPoints; ++i) {
                if (touchEvent.touchPoints[i].id == g_activePointerId) {
                    const evk::ui::PointerEvent event{
                        evk::ui::PointerAction::Move, g_activePointerId,
                        touchEvent.touchPoints[i].x, touchEvent.touchPoints[i].y,
                        touchEvent.timeStamp};
                    evk::ui::dispatchPointerEvent(event);
                    return;
                }
            }
            return;
        }
        case OH_NATIVEXCOMPONENT_UP:
            if (touchEvent.id != g_activePointerId) {
                return;
            }
            g_activePointerId = kNoPointer;
            break;
        case OH_NATIVEXCOMPONENT_CANCEL:
            if (g_activePointerId == kNoPointer) {
                return;
            }
            g_activePointerId = kNoPointer;
            break;
        default:
            return;
    }

    const evk::ui::PointerEvent event{toPointerAction(touchEvent.type), touchEvent.id,
                                      touchEvent.x, touchEvent.y, touchEvent.timeStamp};
    evk::ui::dispatchPointerEvent(event);
}

// ---------------------------------------------------------------------------
// ArkTS 经 onLoad(context) 调用的导出函数
// ---------------------------------------------------------------------------

// ArkTS: bridge.setStoragePath(path)
// 引擎启动最早时刻注入私有存储目录（UIAbility 的 filesDir），core 的 KeyValueStore
// 由此完成 MMKV 初始化；幂等，surface 重建重复调用安全。
static napi_value SetStoragePath(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    // 两段式读字符串：先取长度，再读到已分配的缓冲。
    size_t length = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok) {
        return nullptr;
    }
    std::string path(length, '\0');
    napi_get_value_string_utf8(env, argv[0], path.data(), length + 1, &length);
    evk::KeyValueStore::initialize(path);
    return nullptr;
}

// ArkTS: bridge.beginFrame(frameTimeNanos)
// ArkTS DisplaySync 的帧回调（UI 线程）。core 仅在 dirty 时真正构建和提交帧。
static napi_value BeginFrame(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    int64_t frameTimeNanos = 0;
    napi_get_value_int64(env, argv[0], &frameTimeNanos);
    evk::beginFrame(frameTimeNanos);
    return nullptr;
}

// ArkTS: bridge.onBackPressed(): boolean
// 系统返回统一经事件通道交给 App 决定：消费（导航栈 pop）返回 true；
// 未消费（栈底/无 UI）返回 false，ArkTS 侧据此交还系统默认行为。
static napi_value OnBackPressed(napi_env env, napi_callback_info /*info*/) {
    const bool consumed = evk::dispatchEvent(evk::EventId::BackPressed, nullptr);
    napi_value result = nullptr;
    napi_get_boolean(env, consumed, &result);
    return result;
}

// ArkTS: bridge.safeAreaChanged(top, bottom, left, right)
// 系统窗口安全区（状态栏/刘海/手势条）变化，单位像素，与 surface 坐标系一致。
static napi_value SafeAreaChanged(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return nullptr;
    }
    double insets[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) {
        napi_get_value_double(env, argv[i], &insets[i]);
    }
    evk::SafeAreaData data{static_cast<float>(insets[0]), static_cast<float>(insets[1]),
                           static_cast<float>(insets[2]), static_cast<float>(insets[3])};
    evk::dispatchEvent(evk::EventId::SafeAreaChanged, &data);
    return nullptr;
}

// ---------------------------------------------------------------------------
// 模块注册（规则 1）
// ---------------------------------------------------------------------------

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    if (!env || !exports) {
        return nullptr;
    }
    // 职责①：挂上 ArkTS 可见的函数表。
    napi_property_descriptor desc[] = {
        {"setStoragePath", nullptr, SetStoragePath, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"beginFrame", nullptr, BeginFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onBackPressed", nullptr, OnBackPressed, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"safeAreaChanged", nullptr, SafeAreaChanged, nullptr, nullptr, nullptr, napi_default,
         nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // 职责②：解包 XComponent 实例并注册回调。exports 由 XComponent 框架构造时
    // 才带 __NATIVE_XCOMPONENT_OBJ__；纯 import 加载没有它，跳过即可。
    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) !=
            napi_ok ||
        !exportInstance) {
        return exports;
    }
    OH_NativeXComponent* nativeXComponent = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&nativeXComponent)) !=
            napi_ok ||
        !nativeXComponent) {
        return exports;
    }
    // 静态存储期：RegisterCallback 只存指针，回调表必须比注册点活得久。
    static OH_NativeXComponent_Callback callback = {
        OnSurfaceCreatedCB, OnSurfaceChangedCB, OnSurfaceDestroyedCB, DispatchTouchEventCB,
    };
    OH_NativeXComponent_RegisterCallback(nativeXComponent, &callback);
    return exports;
}
EXTERN_C_END

static napi_module g_estarxModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,        // 模块加载回调：挂函数 + 注册 XComponent 回调
    .nm_modname = "estarx_vulkan",   // 必须等于 XComponent 的 libraryname
    .nm_priv = nullptr,
    .reserved = {0},
};

// so 加载（dlopen）时构造函数自动执行，把模块登记进 NAPI 注册表。
extern "C" __attribute__((constructor)) void RegisterEstarxVulkanModule(void) {
    napi_module_register(&g_estarxModule);
}
