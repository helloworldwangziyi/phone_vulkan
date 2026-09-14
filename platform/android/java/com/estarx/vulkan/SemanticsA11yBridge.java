package com.estarx.vulkan;

import android.content.Context;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.ref.WeakReference;
import java.util.HashMap;

/**
 * 无障碍语义桥：core 语义树快照 ↔ Android 无障碍框架之间的平台侧适配层。
 *
 * 数据流（全部在 UI 线程，core 的无锁单线程假设原样成立，不加锁）：
 *   出向（引擎→平台）：core 每帧 diff 语义树，有变化时经平台通道推全量快照
 *     invokePlatform("a11y/update", json)，JNI 落到 NativeBridge.onPlatformInvoke，
 *     按 "a11y/" 前缀转进本类。快照整体替换节点缓存（UI 规模几十节点，全量
 *     替换比增量合并简单得多），然后向「当前 VulkanSurfaceView」发
 *     TYPE_WINDOW_CONTENT_CHANGED，TalkBack 据此重新走虚拟节点树。
 *   入向（平台→引擎）：两个通道——
 *     a11y/enabled：TalkBack（触摸浏览）开关，surfaceCreated 后推初始态，
 *       之后经 TouchExplorationStateChangeListener 跟随系统变化；
 *     a11y/action：TalkBack 双击/滚动等动作下行，由 VulkanSurfaceView 的
 *       AccessibilityNodeProvider.performAction 直接 dispatch，不经本类。
 *
 * JSON 格式（core 序列化，core/src/ui/semantics.cpp 的 serializeJson）：
 *   {"change":N,"nodes":[{"id":1,"label":"...","hint":"...","role":2,
 *     "actions":3,"state":0,"rect":[x,y,w,h],"children":[2,3]}]}
 *   id 0 恒为合成根（不对应真实控件）；change=-1 表示无障碍关闭，清空缓存。
 *
 * 「当前 View 实例」用静态弱引用持有：surfaceCreated 登记、surfaceDestroyed
 * 清除。引擎重建（退后台再回来）id 从 1 重新分配，旧缓存必须清掉，否则
 * 新帧 id 会别名到旧节点上。
 */
final class SemanticsA11yBridge {

    private SemanticsA11yBridge() {}

    /** 语义节点缓存条目：字段与 core 的 SemanticsNode 一一对应。 */
    static final class Node {
        int id;
        String label = "";
        String hint = "";
        int role;       // 0=容器 1=文本 2=按钮 3=图片 4=标题 5=列表 6=可滚动
        int actions;    // 位掩码：1=tap 2=longPress 4=scrollForward 8=scrollBackward
        int state;      // 位掩码：1=disabled 2=selected
        // rect：surface 像素、原点 surface 左上角（换算屏幕坐标由 View 侧加偏移）。
        float x;
        float y;
        float w;
        float h;
        int[] children = new int[0];
    }

    // 节点缓存：id → Node。只被 UI 线程读写（帧构建尾部回调、
    // AccessibilityNodeProvider 回调都在 UI 线程）。
    private static final HashMap<Integer, Node> sNodes = new HashMap<>();

    // 当前活跃的 VulkanSurfaceView（弱引用防 Activity 泄漏）；surface 销毁后为 null。
    private static WeakReference<VulkanSurfaceView> sActiveView;

    // TalkBack 开关监听；surfaceDestroyed 时移除，防回调打到已销毁引擎。
    private static AccessibilityManager.TouchExplorationStateChangeListener
            sTouchExplorationListener;

    // ---- 出向：平台通道 "a11y/" 前缀方法的总入口（NativeBridge 转入） ----

    static String onInvoke(String method, String args) {
        if ("a11y/update".equals(method)) {
            applyUpdate(args);
        }
        // a11y 前缀的方法不向 App 的 PlatformHandler 转发；未识别的按未实现处理。
        return "";
    }

    // ---- surface 生命周期（VulkanSurfaceView 回调里登记/清除） ----

    // surfaceCreated 在 nativeInit 之后调用：此时 core 已注册 a11y/enabled
    // 处理器（runApp 保证先于首帧），初始态推送不会落空。
    static void onSurfaceReady(VulkanSurfaceView view) {
        sActiveView = new WeakReference<>(view);
        sNodes.clear();  // 引擎重建，id 重新从 1 分配，旧缓存作废
        final AccessibilityManager am = (AccessibilityManager)
                view.getContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
        if (am == null) {
            return;
        }
        sTouchExplorationListener = enabled -> pushEnabled(enabled);
        am.addTouchExplorationStateChangeListener(sTouchExplorationListener);
        pushEnabled(am.isTouchExplorationEnabled());
    }

    static void onSurfaceDestroyed(VulkanSurfaceView view) {
        if (sTouchExplorationListener != null) {
            AccessibilityManager am = (AccessibilityManager)
                    view.getContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
            if (am != null) {
                am.removeTouchExplorationStateChangeListener(sTouchExplorationListener);
            }
            sTouchExplorationListener = null;
        }
        if (sActiveView != null && sActiveView.get() == view) {
            sActiveView = null;
        }
        sNodes.clear();
    }

    private static void pushEnabled(boolean enabled) {
        NativeBridge.nativeDispatchPlatformCall("a11y/enabled", enabled ? "1" : "0");
    }

    // ---- 节点查询（VulkanSurfaceView 的 AccessibilityNodeProvider 用） ----

    static Node getNode(int id) {
        return sNodes.get(id);
    }

    // ---- 快照解析与缓存替换 ----

    private static void applyUpdate(String json) {
        // 拿不到当前 View 实例（如 surface 已销毁）就丢弃本帧。
        VulkanSurfaceView view = sActiveView != null ? sActiveView.get() : null;
        if (view == null) {
            return;
        }
        final JSONObject root;
        final JSONArray nodes;
        try {
            root = new JSONObject(json);
            nodes = root.optJSONArray("nodes");
        } catch (JSONException e) {
            return;  // 非法 JSON 丢弃本帧，不动缓存
        }
        // change=-1（关闭无障碍）时 nodes 为空，整体替换天然等于清空缓存，
        // 无需单独分支；快照语义本就是全量替换。
        sNodes.clear();
        if (nodes != null) {
            for (int i = 0; i < nodes.length(); ++i) {
                Node node = parseNode(nodes.optJSONObject(i));
                if (node != null) {
                    sNodes.put(node.id, node);
                }
            }
        }
        view.sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
    }

    private static Node parseNode(JSONObject obj) {
        if (obj == null) {
            return null;
        }
        Node node = new Node();
        node.id = obj.optInt("id", -1);
        if (node.id < 0) {
            return null;
        }
        node.label = obj.optString("label", "");
        node.hint = obj.optString("hint", "");
        node.role = obj.optInt("role", 0);
        node.actions = obj.optInt("actions", 0);
        node.state = obj.optInt("state", 0);
        JSONArray rect = obj.optJSONArray("rect");
        if (rect != null && rect.length() == 4) {
            node.x = (float) rect.optDouble(0, 0);
            node.y = (float) rect.optDouble(1, 0);
            node.w = (float) rect.optDouble(2, 0);
            node.h = (float) rect.optDouble(3, 0);
        }
        JSONArray children = obj.optJSONArray("children");
        if (children != null && children.length() > 0) {
            int[] ids = new int[children.length()];
            for (int i = 0; i < children.length(); ++i) {
                ids[i] = children.optInt(i, 0);
            }
            node.children = ids;
        }
        return node;
    }
}
