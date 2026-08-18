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
}
