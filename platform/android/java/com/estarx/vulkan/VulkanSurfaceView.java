package com.estarx.vulkan;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * A SurfaceView that forwards surface lifecycle events to the native Vulkan renderer.
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    public VulkanSurfaceView(Context context) {
        super(context);
        init();
    }

    public VulkanSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.nativeInit(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(width, height);
        NativeBridge.nativeRender();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.nativeDestroy();
    }
}
