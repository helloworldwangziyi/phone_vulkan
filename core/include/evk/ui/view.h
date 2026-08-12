#pragma once

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

// 视图树节点。estarx 风格：单一 View 类，任何视图都可有子视图。
class View {
public:
    esx_view handle = 0;
    Rect rect;                    // 相对父视图左上角，像素
    bool visible = true;
    bool hasBackground = false;
    Color background;
    View* parent = nullptr;
    std::vector<std::unique_ptr<View>> children;
    float actualX = 0, actualY = 0; // 相对屏幕左上角，由 updateActuals 重算

    esx_view_draw_func drawFunc = nullptr;
    void* drawUserData = nullptr;
    esx_view_click_func clickFunc = nullptr;
    void* clickUserData = nullptr;
    esx_view_pan_func panFunc = nullptr;
    void* panUserData = nullptr;

    // SDK 控件使用的原始 Pointer 钩子和随 View 生命周期释放的控件状态。
    PointerHandler pointerHandler = nullptr;
    void* pointerUserData = nullptr;
    void (*boundsChangedHandler)(esx_view view, void* userData) = nullptr;
    void* boundsChangedUserData = nullptr;
    const void* controlType = nullptr;
    std::shared_ptr<void> controlState;

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
