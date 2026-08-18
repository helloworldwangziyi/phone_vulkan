// ============================================================================
// JNI 实现层：Java 薄壳(NativeBridge)与 core 之间的翻译层。
//
// 本文件即 JNI 书写规则的范例，规则逐条如下：
//
// 1. 命名约定（静态注册）：函数名 = Java_<包名>_<类名>_<方法名>，
//    包名中的 . 换成 _，方法名中的 _ 要转义成 _1。
//    例：com.estarx.vulkan.NativeBridge.nativeInit
//        → Java_com_estarx_vulkan_NativeBridge_nativeInit。
//    JVM 调用 native 方法时按此名字在 .so 里查找符号，
//    名字对不上会在首次调用时抛 UnsatisfiedLinkError。
//
// 2. extern "C" 必须加：C++ 默认对函数名做 name mangling（修饰），
//    加上后符号保持原样，JVM 才能按名字找到。
//
// 3. JNIEXPORT / JNICALL 固定搭配：前者保证符号从动态库导出可见，
//    后者声明平台调用约定（Android 上为空宏，照抄即可）。
//
// 4. 前两个参数由 JVM 固定传入：
//    - JNIEnv* env：JNI 操作句柄（调 Java 方法、读写 Java 对象全靠它），
//      仅在当前调用线程有效，绝不能保存下来跨线程使用；
//    - 第二参数：Java 侧是 static 方法时为 jclass（类本身），
//      实例方法时为 jobject（即 this）。本文件全是 static，故都是 jclass。
//    从第三个参数起才一一对应 Java 声明里的参数。
//
// 5. 类型映射：jint=int32_t，jfloat=float，jboolean=uint8_t，
//    jlong=int64_t；jstring 不是 char*，字符串要用
//    GetStringUTFChars 取出、ReleaseStringUTFChars 归还；
//    任何 Java 对象传进来都是 jobject 引用，只能靠 JNI 函数访问。
//
// 6. 线程规则：本文件所有函数都运行在 Android UI 线程。
//    若将来后台线程要调 JNI，必须先 AttachCurrentThread 拿到该线程
//    自己的 JNIEnv，用完 DetachCurrentThread。
//
// 7. 备选方案（动态注册）：在 JNI_OnLoad 里用 RegisterNatives 以函数表
//    绑定 Java 方法与 C 函数（estarx 的 jni/jni_export.c 就是这种），
//    适合方法数量多、不想受命名约定约束的场景；本项目方法少，
//    静态命名约定更直观，故未采用。
//
// 设计约定：本层只做"解包参数 → 转发 core"，不写任何业务逻辑；
// 事件经 evk::dispatchEvent 回流给 App 注册的 EventFunc（见 app_lifecycle.h）。
// ============================================================================

#include <jni.h>

#include <spdlog/sinks/android_sink.h>

#include "evk/app_lifecycle.h"
#include "evk/kv_store.h"
#include "evk/log.h"
#include "evk/frame_scheduler.h"
#include "evk/vulkan_renderer.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/paint_canvas.h"
#include "evk/ui/pointer_input.h"

// android_vulkan_platform.cpp 以 C 链接导出的工厂函数（规则 2 的同款应用）：
// bridge 只需声明原型即可调用，无需知道 AndroidPlatform 类的布局。
extern "C" evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void evkDestroyAndroidPlatform(evk::IPlatform* platform);

// 全局渲染器与平台适配器，生命周期与 surface 一致（nativeInit 建 / nativeDestroy 拆）。
static evk::Renderer* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;
static bool g_appStarted = false;
static bool g_firstFrameLogged = false;

// 画一帧：构建视图树内容（内部执行 View draw callback）后交给渲染器。
// 同时注册为 core 的 FrameFunc，App 调 evk::requestRender() 会走到这里。
static void renderFrame(int64_t /*frameTimeNanos*/) {
    static evk::ui::Canvas canvas;
    evk::ui::buildFrame(canvas);
    if (!g_firstFrameLogged) {
        g_firstFrameLogged = true;
        EVK_LOGI("first UI frame: vertices={}, batches={}",
                 canvas.vertices().size(), canvas.batches().size());
    }
    if (g_renderer) {
        g_renderer->render(canvas);
    }
}

// Java: NativeBridge.nativeSetStoragePath(String)
// 引擎启动最早时刻注入私有存储目录（filesDir），core 的 KeyValueStore
// 由此完成 MMKV 初始化；幂等，surface 重建重复调用安全。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeSetStoragePath(JNIEnv* env, jclass /*clazz*/, jstring path) {
    if (!path) {
        return;
    }
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (!chars) {
        return; // OOM，JVM 已抛异常
    }
    evk::KeyValueStore::initialize(chars);
    env->ReleaseStringUTFChars(path, chars);
}

// Java: NativeBridge.nativeInit(Surface)
// surface 创建就绪时调用（可能多次：退后台重建）。幂等：已有渲染器直接返回。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass /*clazz*/, jobject surface) {
    // logcat sink 由平台壳注入；core 默认只提供 stdout（见 evk/log.h）。
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::android_logger_mt("estarx-vulkan", "estarx-vulkan");
    }
    evk::log::init(logger);
    if (g_renderer) {
        return;
    }

    g_platform = evkCreateAndroidPlatform(env, surface);
    if (!g_platform) {
        EVK_LOGE("failed to create Android platform");
        return;
    }

    g_renderer = new evk::Renderer(g_platform);
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 surfaceCreated 能干净重试。
        EVK_LOGE("failed to initialize Vulkan renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    EVK_LOGI("Vulkan renderer initialized");
    // 平台壳的"画一帧"实现注册给 core，App 的 requestRender() 由此触发。
    evk::setFrameFunc(&renderFrame);
    // 渲染器初始化完成：core 进入就绪状态，之后 App 才能安全创建视图。
    evk::setEngineReady(true);
    // EngineReady 之前先报一次 SurfaceChanged：initialize() 成功后 ANativeWindow 已建立，
    // 取到的是 surface 的真实像素尺寸，App 建视图树时即可按真实像素布局。
    // 之后的尺寸变化仍由 nativeResize 通道上报。
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    g_platform->getSurfaceSize(&surfaceWidth, &surfaceHeight);
    evk::SurfaceChangedData initSize{ static_cast<int32_t>(surfaceWidth),
                                      static_cast<int32_t>(surfaceHeight) };
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &initSize);
    if (!g_appStarted) {
        g_appStarted = true;
        evk::dispatchEvent(evk::EventId::EngineReady, nullptr);
    }
    evk::requestRender();
}

// Java: NativeBridge.nativeResize(int, int)
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(JNIEnv* /*env*/, jclass /*clazz*/, jint width, jint height) {
    evk::SurfaceChangedData data{ width, height };
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &data);
    if (g_renderer) {
        g_renderer->setSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
    evk::requestRender();
}

// Java Choreographer 的 VSync 回调。core 仅在 dirty 时真正构建和提交帧。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeBeginFrame(JNIEnv* /*env*/, jclass /*clazz*/,
                                                     jlong frameTimeNanos) {
    evk::beginFrame(static_cast<int64_t>(frameTimeNanos));
}

// Java: NativeBridge.nativeOnTouch(int, int, float, float, long)
// 本层只把 Android MotionEvent 动作翻译成跨平台 PointerAction；
// eventTimeNanos 来自 MotionEvent.getEventTime()，供 core 计算滑动速度。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeOnTouch(JNIEnv* /*env*/, jclass /*clazz*/,
                                                  jint action, jint pointerId,
                                                  jfloat x, jfloat y,
                                                  jlong eventTimeNanos) {
    evk::ui::PointerAction pointerAction;
    switch (action) {
        case 0: pointerAction = evk::ui::PointerAction::Down; break;
        case 1: pointerAction = evk::ui::PointerAction::Up; break;
        case 2: pointerAction = evk::ui::PointerAction::Move; break;
        case 3: pointerAction = evk::ui::PointerAction::Cancel; break;
        default: return;
    }
    const evk::ui::PointerEvent event{pointerAction, pointerId, x, y,
                                      static_cast<int64_t>(eventTimeNanos)};
    evk::ui::dispatchPointerEvent(event);
}

// Java: NativeBridge.nativeOnBackPressed()
// 系统返回统一经事件通道交给 App 决定：消费（导航栈 pop）返回 true；
// 未消费（栈底/无 UI）返回 false，Java 侧据此交还系统默认行为。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeOnBackPressed(JNIEnv* /*env*/, jclass /*clazz*/) {
    return evk::dispatchEvent(evk::EventId::BackPressed, nullptr) ? JNI_TRUE : JNI_FALSE;
}

// Java: NativeBridge.nativeDestroy()
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    evk::cancelPendingFrame();
    evk::setFrameFunc(nullptr);
    evk::setEngineReady(false);
    g_appStarted = false; // 下次 surfaceCreated 走完整 EngineReady 重建流程
    g_firstFrameLogged = false;
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
    if (g_platform) {
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("Vulkan renderer destroyed");
}
