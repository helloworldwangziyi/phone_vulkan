/**
 * @file view.cpp
 * @brief 视图树节点（View）与 Rect/Color 基础类型的实现。
 */
#include "evk/ui/view.h"

#include <algorithm>

namespace evk::ui {

bool Rect::contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
}

Rect Rect::intersect(const Rect& a, const Rect& b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1) {
        return {x1, y1, 0, 0};
    }
    return {x1, y1, x2 - x1, y2 - y1};
}

Color Color::rgba(uint32_t v) {
    Color c;
    c.r = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
    c.g = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    c.b = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    c.a = static_cast<float>(v & 0xFF) / 255.0f;
    return c;
}

/**
 * @brief 挂载子视图：所有权（unique_ptr）移交给父视图，返回裸指针供调用方临时使用。
 *
 * children 的顺序即绘制顺序（越靠后越晚画、越在上层），也是 hitTest 的优先序。
 * 注意返回的裸指针在后续树变更（如 reconcile 重建）后可能失效，不要长期持有。
 */
View* View::addChild(std::unique_ptr<View> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

/**
 * @brief 前序遍历重算屏幕绝对坐标：actual = parent.actual + rect（根视图 = rect）。
 *
 * 布局只写局部 rect（相对父左上角），绘制与命中测试用的都是 actual 坐标，
 * 所以任何 rect 变更后、绘制/命中测试前都必须先调本函数——
 * esxBuildFrame 与 dispatchPointerEvent 开头各调一次即是为此。
 * 目前每次全树重算（O(节点数)），当前规模足够便宜；将来可换脏标记增量更新。
 */
void View::updateActuals() {
    if (parent) {
        actualX = parent->actualX + rect.x;
        actualY = parent->actualY + rect.y;
    } else {
        actualX = rect.x;
        actualY = rect.y;
    }
    for (auto& child : children) {
        child->updateActuals();
    }
}

/**
 * @brief 屏幕坐标命中测试：返回"最深的命中节点"（输入层再沿父链向上认领目标）。
 *
 * 1) 自身不可见或点不在自身 actual 矩形内 → 整棵子树都不命中，直接返回；
 * 2) 子节点倒序遍历（最后添加的 child 最后绘制 = 最上层）优先命中——
 *    与 drawViewTree 的绘制顺序一致，保证"看得见谁就点到谁"；
 * 3) 子节点都不命中才返回 this（点落在自己身上）。
 *
 * 注意 px/py 是屏幕坐标，比较对象是 actualRect 而非 rect（局部坐标）。
 */
View* View::hitTest(float px, float py) {
    if (!visible || !actualRect().contains(px, py)) {
        return nullptr;
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (View* hit = (*it)->hitTest(px, py)) {
            return hit;
        }
    }
    return this;
}

/**
 * @brief 点是否"同时"位于自身及整条父链的可见矩形内（屏幕坐标）。
 *
 * 比 hitTest 严格：hitTest 只回答"谁在该点最上层"，
 * 本函数回答"该视图自己是否真正可见地包含该点"——
 * 用途：Up 时判定点击是否仍有效（手指滑出视图或被上层盖住则不算），
 *      以及 Button 的 Move 移入/移出按下态判定。
 */
bool View::containsVisiblePoint(float px, float py) const {
    for (const View* view = this; view; view = view->parent) {
        if (!view->visible || !view->actualRect().contains(px, py)) {
            return false;
        }
    }
    return true;
}

} // namespace evk::ui
