#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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
    Rect rect;                    // 相对父视图左上角，像素
    bool visible = true;
    bool hasBackground = false;
    Color background;
    View* parent = nullptr;
    std::vector<std::unique_ptr<View>> children;
    float actualX = 0, actualY = 0; // 相对屏幕左上角，由 updateActuals 重算

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
};

} // namespace evk::ui
