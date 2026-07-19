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

    public static native void nativeInit(Surface surface);
    public static native void nativeResize(int width, int height);
    public static native void nativeRender();
    public static native void nativeDestroy();
}
