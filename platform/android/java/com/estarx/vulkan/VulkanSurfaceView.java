package com.estarx.vulkan;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

/**
 * A SurfaceView that forwards surface lifecycle events to the native Vulkan renderer.
 *
 * 薄壳视图：只负责向系统拿 surface、收触摸，然后原样转发给 JNI。
 * 自身不含任何业务逻辑（"薄壳"的含义）。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    private static final int NO_POINTER = -1;

    private boolean surfaceReady;
    private int activePointerId = NO_POINTER;
    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (!surfaceReady) {
                return;
            }
            NativeBridge.nativeBeginFrame(frameTimeNanos);
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

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
        // 安全区上报：主题是 windowFullscreen，视图延伸到状态栏/手势条之下，
        // 必须把系统占用区（含刘海 cutout）通知 App 做布局内缩。
        // inset 值单位是像素，与 surface 坐标系一致；不 consume，交还系统继续分发。
        ViewCompat.setOnApplyWindowInsetsListener(this, (view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                            | WindowInsetsCompat.Type.displayCutout());
            NativeBridge.nativeSafeAreaChanged(bars.top, bars.bottom, bars.left, bars.right);
            return windowInsets;
        });
    }

    // 画板被系统创建好时回调（View 显示到屏幕、或退后台再回来时重建）。
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // 先于 nativeInit 注入私有存储目录：core 的 KeyValueStore 初始化
        // 需要平台路径，引擎就绪（EngineReady 建视图树）前必须完成。
        // filesDir 是 App 沙盒私有目录，卸载即清、无需权限。
        NativeBridge.nativeSetStoragePath(getContext().getFilesDir().getAbsolutePath());
        // holder.getSurface() 就是 Java 层的画板句柄，
        // 传给 native，C++ 用它创建 Vulkan 渲染表面。
        NativeBridge.nativeInit(holder.getSurface());
        surfaceReady = true;
        Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    // 画板尺寸确定或变化时回调（首次创建后紧跟一次；旋转屏幕等也会触发）。
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(width, height); // 告诉 native 画板新尺寸
    }

    // 画板被销毁时回调（退后台、Activity 销毁），通知 native 释放渲染资源。
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        activePointerId = NO_POINTER;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        NativeBridge.nativeDestroy();
    }

    // 手指触摸到这块 View 时系统回调，按下/移动/抬起都会进来。
    // getEventTime() 是事件发生时刻（毫秒），换算成纳秒传给 native 计算滑动速度。
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        final long eventTimeNanos = event.getEventTime() * 1_000_000L;
        if (action == MotionEvent.ACTION_DOWN) {
            activePointerId = event.getPointerId(0);
            NativeBridge.nativeOnTouch(action, activePointerId, event.getX(0),
                    event.getY(0), eventTimeNanos);
        } else if (action == MotionEvent.ACTION_MOVE && activePointerId != NO_POINTER) {
            final int pointerIndex = event.findPointerIndex(activePointerId);
            if (pointerIndex >= 0) {
                NativeBridge.nativeOnTouch(action, activePointerId,
                        event.getX(pointerIndex), event.getY(pointerIndex), eventTimeNanos);
            }
        } else if ((action == MotionEvent.ACTION_UP ||
                    action == MotionEvent.ACTION_POINTER_UP) &&
                   event.getPointerId(event.getActionIndex()) == activePointerId) {
            final int pointerIndex = event.getActionIndex();
            NativeBridge.nativeOnTouch(MotionEvent.ACTION_UP, activePointerId,
                    event.getX(pointerIndex), event.getY(pointerIndex), eventTimeNanos);
            activePointerId = NO_POINTER;
        } else if (action == MotionEvent.ACTION_CANCEL && activePointerId != NO_POINTER) {
            NativeBridge.nativeOnTouch(action, activePointerId, 0.0f, 0.0f, eventTimeNanos);
            activePointerId = NO_POINTER;
        }
        // return true  = "这个事件我处理了"，后续的 MOVE/UP 才会继续发给我；
        // return false = 不感兴趣，系统只给这一次 DOWN，之后不再送来。
        return true;
    }
}
