package com.estarx.vulkan;

import android.content.Context;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

/**
 * A SurfaceView that forwards surface lifecycle events to the native Vulkan renderer.
 *
 * 薄壳视图：只负责向系统拿 surface、收触摸，然后原样转发给 JNI。
 * 自身不含任何业务逻辑（"薄壳"的含义）。
 *
 * 无障碍：core 的语义树以「虚拟节点」形式暴露给 TalkBack——宿主 View 对应
 * 合成根（id 0），每个语义节点一个 virtualId（= core 侧节点 id），由
 * SemanticsNodeProvider 从 SemanticsA11yBridge 的快照缓存即时构建
 * AccessibilityNodeInfo；动作（双击/长按/滚动）经平台通道 a11y/action 下行回
 * core，与触摸回调同路。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    private boolean surfaceReady;
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
        // 无障碍：SurfaceView 默认对无障碍服务不重要，必须显式标记，
        // TalkBack 才会走 getAccessibilityNodeProvider() 拿虚拟节点树。
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
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
        // 无障碍桥登记「当前 View 实例」并推一次 TalkBack 初始开关态：
        // 必须在 nativeInit 之后——core 的 a11y/enabled 处理器在 runApp 时
        // 注册（先于首帧），EngineReady 事件在 nativeInit 内同步派发，
        // 此时推送不会落空。
        SemanticsA11yBridge.onSurfaceReady(this);
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
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        NativeBridge.nativeDestroy();
        // 引擎已销毁：清除「当前 View 实例」登记、移除 TalkBack 开关监听、
        // 清节点缓存（引擎重建后 id 从 1 重排，旧缓存会别名到新节点）。
        SemanticsA11yBridge.onSurfaceDestroyed(this);
    }

    // 手指触摸到这块 View 时系统回调。多点触控全量转发：
    // DOWN/POINTER_DOWN 报落指的 id；MOVE 批量携带全部触点，逐 id 转发；
    // UP/POINTER_UP 报抬指的 id；CANCEL 对当前所有触点逐个补发。
    // core 侧按 pointerId 分指跟踪，单指与多指手势走同一状态表。
    // getEventTime() 是事件发生时刻（毫秒），换算成纳秒传给 native 计算滑动速度。
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        final long eventTimeNanos = event.getEventTime() * 1_000_000L;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                final int index = event.getActionIndex();
                NativeBridge.nativeOnTouch(MotionEvent.ACTION_DOWN,
                        event.getPointerId(index), event.getX(index),
                        event.getY(index), eventTimeNanos);
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    NativeBridge.nativeOnTouch(MotionEvent.ACTION_MOVE,
                            event.getPointerId(i), event.getX(i),
                            event.getY(i), eventTimeNanos);
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                final int index = event.getActionIndex();
                NativeBridge.nativeOnTouch(MotionEvent.ACTION_UP,
                        event.getPointerId(index), event.getX(index),
                        event.getY(index), eventTimeNanos);
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    NativeBridge.nativeOnTouch(MotionEvent.ACTION_CANCEL,
                            event.getPointerId(i), event.getX(i),
                            event.getY(i), eventTimeNanos);
                }
                break;
            }
            default:
                break;
        }
        // return true  = "这个事件我处理了"，后续的 MOVE/UP 才会继续发给我；
        // return false = 不感兴趣，系统只给这一次 DOWN，之后不再送来。
        return true;
    }

    // ---- 无障碍虚拟节点（core 语义树 → TalkBack，缓存见 SemanticsA11yBridge） ----

    // 懒创建：TalkBack 未开启时系统不会调本方法，不白付对象成本。
    private AccessibilityNodeProvider a11yProvider;

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        if (a11yProvider == null) {
            a11yProvider = new SemanticsNodeProvider();
        }
        return a11yProvider;
    }

    // 宿主 View 自身的节点信息：super 填好 SurfaceView 的默认属性后，
    // 把合成根（id 0）的 children 挂为虚拟子节点——它们是 TalkBack 遍历的
    // 顶层入口（core 侧合成根的 children 即顶层语义节点 id 列表）。
    @Override
    public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        SemanticsA11yBridge.Node root = SemanticsA11yBridge.getNode(0);
        if (root != null) {
            for (int childId : root.children) {
                info.addChild(this, childId);
            }
        }
    }

    /**
     * 虚拟节点 Provider：TalkBack 按 virtualId 索取节点信息/下发动作。
     * virtualId 与 core 语义节点 id 同空间（HOST_VIEW_ID=-1 表示宿主自身）。
     * 所有回调都在 UI 线程，直接读 SemanticsA11yBridge 的缓存，无需加锁。
     */
    private final class SemanticsNodeProvider extends AccessibilityNodeProvider {

        @Override
        public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualId) {
            if (virtualId == AccessibilityNodeProvider.HOST_VIEW_ID) {
                // 宿主自身：setSource 关联本 View 后走 onInitialize 填默认属性
                // 和顶层虚拟子节点（与系统直接回调宿主时同一份逻辑）。
                AccessibilityNodeInfo info = newNodeInfo();
                info.setSource(VulkanSurfaceView.this);
                onInitializeAccessibilityNodeInfo(info);
                return info;
            }
            SemanticsA11yBridge.Node node = SemanticsA11yBridge.getNode(virtualId);
            if (node == null) {
                return null;  // 帧间窗口：节点刚被删，TalkBack 拿到 null 会跳过
            }
            AccessibilityNodeInfo info = newNodeInfo();
            info.setPackageName(getContext().getPackageName());
            info.setSource(VulkanSurfaceView.this, virtualId);
            info.setParent(VulkanSurfaceView.this);
            info.setVisibleToUser(true);
            info.setFocusable(true);
            // 朗读内容 = label，hint 非空拼接在后（双击提示等）。
            String text = node.label;
            if (!node.hint.isEmpty()) {
                text = text.isEmpty() ? node.hint : text + "，" + node.hint;
            }
            info.setContentDescription(text);
            info.setClassName(classNameForRole(node.role));
            if (node.role == 4 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                info.setHeading(true);  // 标题角色：API 28+ 供 TalkBack 按标题导航
            }
            // 屏幕坐标 = surface 相对坐标 + View 在屏幕上的偏移（surface 像素
            // 与 View 像素 1:1，原点都在各自左上角）。
            int[] location = new int[2];
            getLocationOnScreen(location);
            info.setBoundsInScreen(new Rect(
                    Math.round(node.x) + location[0],
                    Math.round(node.y) + location[1],
                    Math.round(node.x + node.w) + location[0],
                    Math.round(node.y + node.h) + location[1]));
            // 动作位 → 系统标准动作（位定义与 core 的 kSemanticsAction* 一致）。
            if ((node.actions & 1) != 0) {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
            }
            if ((node.actions & 2) != 0) {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_LONG_CLICK);
            }
            if ((node.actions & 4) != 0) {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
            }
            if ((node.actions & 8) != 0) {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD);
            }
            // 状态位：1=disabled（默认 enabled，仅显式禁用时关掉）2=selected。
            info.setEnabled((node.state & 1) == 0);
            if ((node.state & 2) != 0) {
                info.setSelected(true);
            }
            // 虚拟子节点：children 是 core 侧的孩子 id 列表，直接映射。
            for (int childId : node.children) {
                info.addChild(VulkanSurfaceView.this, childId);
            }
            return info;
        }

        // TalkBack 动作下行：双击（CLICK）、长按、双指滚动等，映射回 core 的
        // 动作位，经平台通道同步执行；"1" 表示 core 已消费（节点声明了该动作
        // 且 View 仍存活）。
        @Override
        public boolean performAction(int virtualId, int action, Bundle arguments) {
            final int actionBit;
            if (action == AccessibilityNodeInfo.ACTION_CLICK) {
                actionBit = 1;
            } else if (action == AccessibilityNodeInfo.ACTION_LONG_CLICK) {
                actionBit = 2;
            } else if (action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) {
                actionBit = 4;
            } else if (action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) {
                actionBit = 8;
            } else {
                return super.performAction(virtualId, action, arguments);
            }
            String result = NativeBridge.nativeDispatchPlatformCall(
                    "a11y/action", virtualId + ":" + actionBit);
            return "1".equals(result);
        }

        // AccessibilityNodeInfo 的创建口：API 33 起 obtain() 工厂全部废弃
        // （官方改为公开构造器，实例不再走缓存池）；旧版本构造器尚未公开，
        // 只能走 obtain()，故压 deprecation。
        // 不能标 static：内部类的静态方法要 Java 16+，本项目源码级别是 11。
        @SuppressWarnings("deprecation")
        private AccessibilityNodeInfo newNodeInfo() {
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                    ? new AccessibilityNodeInfo()
                    : AccessibilityNodeInfo.obtain();
        }

        // core 角色 → Android 控件类名：TalkBack 据此播报控件类型
        // （"按钮"/"列表"等）。0=未指定按普通容器处理。
        private String classNameForRole(int role) {
            switch (role) {
                case 1: return "android.widget.TextView";
                case 2: return "android.widget.Button";
                case 3: return "android.widget.ImageView";
                case 4: return "android.widget.TextView";  // 标题（另见 setHeading）
                case 5: return "android.widget.ListView";
                case 6: return "android.widget.ScrollView";
                default: return "android.view.View";
            }
        }
    }
}
