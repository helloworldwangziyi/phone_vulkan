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
}
