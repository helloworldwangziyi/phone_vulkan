package com.estarx.vulkan;

import android.view.Surface;

/**
 * JNI bridge to the native Vulkan renderer.
 */
public final class NativeBridge {
    static {
        System.loadLibrary("estarx_vulkan");
    }

    private NativeBridge() {}

    // 引擎启动最早时刻把平台私有存储目录传给 native（KeyValueStore 初始化用）。
    public static native void nativeSetStoragePath(String path);

    public static native void nativeInit(Surface surface);
    public static native void nativeResize(int width, int height);
    public static native void nativeBeginFrame(long frameTimeNanos);
    public static native void nativeDestroy();
    public static native void nativeOnTouch(int action, int pointerId, float x, float y,
                                            long eventTimeNanos);

    // 系统返回（返回键/手势导航侧滑）：true = App 已消费（导航栈 pop），
    // false = 栈已在根，调用方应交还系统默认行为（finish Activity）。
    public static native boolean nativeOnBackPressed();

    // 安全区内边距（像素）：系统窗口 inset 变化时由 WindowInsets 监听回调上报。
    public static native void nativeSafeAreaChanged(int top, int bottom, int left, int right);

    // ---- 平台通道（MethodChannel 式，按方法名路由；同步、UI 线程）----

    // 平台→引擎入向：方法名与 UTF-8 参数串交给 core 路由；未注册方法返回空串。
    public static native String nativeDispatchPlatformCall(String method, String args);

    // App 侧的平台能力处理器：引擎出向调用经 onPlatformInvoke 转到这里。
    public interface PlatformHandler {
        String onInvoke(String method, String args);
    }

    private static PlatformHandler sPlatformHandler;

    // App 在启动时注册出向处理器（如 MainActivity.onCreate）。
    public static void setPlatformHandler(PlatformHandler handler) {
        sPlatformHandler = handler;
    }

    // 引擎→平台出向的落点（native 经 JNI 回调进来）；未注册时返回空串。
    // "a11y/" 前缀是无障碍语义通道（core 语义树快照），平台库内部自洽处理，
    // 不占用 App 的 PlatformHandler；其余方法照旧转发给 App。
    public static String onPlatformInvoke(String method, String args) {
        if (method != null && method.startsWith("a11y/")) {
            return SemanticsA11yBridge.onInvoke(method, args);
        }
        PlatformHandler handler = sPlatformHandler;
        return handler != null ? handler.onInvoke(method, args) : "";
    }
}
