package com.estarx.vulkan;

import android.view.Surface;

/**
 * JNI bridge to the native Vulkan renderer.
 */
public final class ZyNativeBridge {
    static {
        System.loadLibrary("estarx_vulkan");
    }

    private ZyNativeBridge() {}

    public static native void nativeInit(Surface surface);
    public static native void nativeResize(int width, int height);
    public static native void nativeBeginFrame(long frameTimeNanos);
    public static native void nativeDestroy();
    public static native void nativeOnTouch(int action, int pointerId, float x, float y,
                                            long eventTimeNanos);
}
