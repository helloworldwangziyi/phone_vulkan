package com.estarx.vulkan;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * A SurfaceView that forwards surface lifecycle events to the native Vulkan renderer.
 *
 * 薄壳视图：只负责向系统拿 surface、收触摸，然后原样转发给 JNI。
 * 自身不含任何业务逻辑（"薄壳"的含义）。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    // 构造函数①：代码里 new VulkanSurfaceView(context) 时走这个。
    public VulkanSurfaceView(Context context) {
        super(context);
        init();
    }

    // 构造函数②：XML 布局反射创建时走这个（本项目 activity_main.xml 走的就是它）。
    // attrs 装着 XML 标签上写的属性（android:id、layout_width 等）。
    // 两个构造都必须提供，否则反射或手动 new 总有一边会崩。
    public VulkanSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        // getHolder() 拿到这块 View 底层画板的管理员(SurfaceHolder)，
        // addCallback(this) 向他登记："画板创建/变化/销毁时请通知我"，
        // 通知会送到下面三个 surfaceXxx 方法（this 就是本对象）。
        getHolder().addCallback(this);
    }

    // 画板被系统创建好时回调（View 显示到屏幕、或退后台再回来时重建）。
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // holder.getSurface() 就是 Java 层的画板句柄，
        // 传给 native，C++ 用它创建 Vulkan 渲染表面。
        NativeBridge.nativeInit(holder.getSurface());
    }

    // 画板尺寸确定或变化时回调（首次创建后紧跟一次；旋转屏幕等也会触发）。
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(width, height); // 告诉 native 画板新尺寸
        NativeBridge.nativeRender();              // 按新尺寸画一帧
    }

    // 画板被销毁时回调（退后台、Activity 销毁），通知 native 释放渲染资源。
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.nativeDestroy();
    }

    // 手指触摸到这块 View 时系统回调，按下/移动/抬起都会进来。
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // getActionMasked() = 动作类型（0=按下 1=抬起 2=移动），
        // getX()/getY() = 触摸点相对本 View 左上角的像素坐标。
        NativeBridge.nativeOnTouch(event.getActionMasked(), event.getX(), event.getY());
        // return true  = "这个事件我处理了"，后续的 MOVE/UP 才会继续发给我；
        // return false = 不感兴趣，系统只给这一次 DOWN，之后不再送来。
        return true;
    }
}
