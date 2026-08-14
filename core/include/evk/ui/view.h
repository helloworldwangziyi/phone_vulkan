#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "evk/esx_view.h"
#include "evk/ui/input.h"

namespace evk::ui {

// 像素矩形。x/y 为左上角。
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    // 点 (px, py) 是否落在矩形内（左闭右开）。
    bool contains(float px, float py) const;

    // 两矩形交集；无交集返回 w=h=0。
    static Rect intersect(const Rect& a, const Rect& b);
};

// RGBA 颜色，分量 0~1。
struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    // 0xRRGGBBAA 打包整数转 Color。
    static Color rgba(uint32_t v);
};

// 视图树节点。estarx 风格：任何视图都可有子视图。
// 普通视图直接使用；SDK 控件（Button/ScrollView/Navigation）继承本类，
// 重写下面的虚函数钩子获得输入/布局行为。
class View {
public:
    virtual ~View() = default;

    esx_view handle = 0;
    Rect rect;                    // 相对父视图左上角，像素
    bool visible = true;
    bool hasBackground = false;
    Color background;
    View* parent = nullptr;
    std::vector<std::unique_ptr<View>> children;
    float actualX = 0, actualY = 0; // 相对屏幕左上角，由 updateActuals 重算

    // App 侧回调（C ABI 绑定）。SDK 控件不使用这些字段。
    esx_view_draw_func drawFunc = nullptr;
    void* drawUserData = nullptr;
    esx_view_click_func clickFunc = nullptr;
    void* clickUserData = nullptr;
    esx_view_pan_func panFunc = nullptr;
    void* panUserData = nullptr;
    // 页面导航生命周期回调，由 Navigation 等导航容器触发。
    esx_view_nav_func navFunc = nullptr;
    void* navUserData = nullptr;

    // ---- SDK 控件行为钩子（继承重写）----
    // 是否想成为原始 Pointer（Down/Move/Up/Cancel）输入目标，如 Button/ScrollView。
    virtual bool acceptsPointerInput() const { return false; }
    // 是否想成为滑动（pan）目标，如 ScrollView/Navigation。
    virtual bool acceptsPanInput() const { return false; }
    virtual void handlePointer(const PointerEvent& /*event*/) {}
    virtual void handlePan(const esx_view_pan_event& /*event*/) {}
    // esx_view_set_bounds 之后调用（如 ScrollView 重 clamp、Navigation 重排）。
    virtual void handleBoundsChanged() {}
    // 子视图经 esx_destroy_view 移除前调用（index 为移除前在 children 中的
    // 下标），供 Flex 等容器同步与子视图平行的数据。
    virtual void handleChildRemoved(size_t /*index*/) {}

    // 挂载子视图，返回子视图裸指针（所有权归父视图）。
    View* addChild(std::unique_ptr<View> child);

    // 前序遍历重算 actual：actual = parent.actual + rect.x/y
    // （根视图 parent=null，actual=rect.x/y）。
    void updateActuals();

    // 命中测试：自身不可见或无命中返回 nullptr；从最上层（最后添加的）
    // child 往下递归，返回最深命中者。px/py 为屏幕坐标。
    View* hitTest(float px, float py);

    // 自身矩形（actual 坐标）。
    Rect actualRect() const { return {actualX, actualY, rect.w, rect.h}; }

    // 点是否同时位于自身及所有可见父视图内。
    bool containsVisiblePoint(float px, float py) const;
};

} // namespace evk::ui
