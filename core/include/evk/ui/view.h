#pragma once

/**
 * @file view.h
 * @brief 视图树节点（View）与 Rect/Color 基础类型。
 */
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "evk/esx_view.h"
#include "evk/ui/input.h"

namespace evk::ui {

/**
 * @brief 像素矩形。x/y 为左上角。
 */
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    /**
     * @brief 点 (px, py) 是否落在矩形内（左闭右开）。
     */
    bool contains(float px, float py) const;

    /**
     * @brief 两矩形交集。
     * @return 交集矩形；无交集返回 w=h=0
     */
    static Rect intersect(const Rect& a, const Rect& b);
};

/**
 * @brief RGBA 颜色，分量 0~1。
 */
struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    /**
     * @brief 0xRRGGBBAA 打包整数转 Color。
     * @param v 0xRRGGBBAA 打包整数
     * @return 分量 0~1 的 Color
     */
    static Color rgba(uint32_t v);
};

/**
 * @brief 视图树节点：retained 渲染对象，一次创建长期存活，重建只更新属性。
 *
 * estarx 风格：任何视图都可有子视图；父子关系 = 所有权关系（children 持有
 * unique_ptr），绘制顺序 = children 顺序（越靠后越晚画、越在上层）。
 * 普通视图直接使用（背景 + draw/click/pan 回调）；SDK 控件（Button/
 * ScrollView/Navigation）继承本类并重写输入/布局钩子获得行为。
 * App 永远只经 esx_view 句柄 + C ABI 操作，不直接持有 View*。
 *
 * 坐标双轨：
 * - rect   —— 布局写的局部坐标（相对父左上角），只有 esx_view_set_bounds 修改；
 * - actual —— 屏幕坐标（updateActuals 前序累加），绘制/命中测试使用。
 */
class View {
public:
    virtual ~View() = default;

    esx_view handle = 0; ///< 注册表句柄（g_handles），0 = 尚未注册/无效
    Rect rect; ///< 相对父视图左上角，像素（布局只写这里）
    bool visible = true; ///< false 时整棵子树不绘制、不命中
    bool hasBackground = false; ///< 是否绘制背景（background 生效）
    Color background; ///< 背景色（0~1 分量，由 0xRRGGBBAA 转换而来）
    View* parent = nullptr; ///< 父节点裸指针（不拥有所有权，父子同树同生死）
    std::vector<std::unique_ptr<View>> children; ///< 子节点（唯一所有权；顺序=层叠序）
    float actualX = 0, actualY = 0; ///< 相对屏幕左上角，由 updateActuals 前序重算

    /**
     * @brief App 侧回调（C ABI 绑定：函数指针 + userData）。
     *
     * SDK 控件不使用这些字段——它们重写 handlePointer/handlePan 虚函数实现
     * 自己的行为。userData 由注册方保证生命周期（声明式层指向 Element 持有的堆槽）。
     */
    esx_view_draw_func drawFunc = nullptr; ///< 帧构建期间调用（自定义绘制）
    void* drawUserData = nullptr;
    esx_view_click_func clickFunc = nullptr; ///< 输入层合成点击后调用（相对坐标）
    void* clickUserData = nullptr;
    esx_view_pan_func panFunc = nullptr; ///< 成为滑动目标后收 pan 事件
    void* panUserData = nullptr;
    esx_view_nav_func navFunc = nullptr; ///< 页面导航生命周期回调，由 Navigation 等导航容器在 push/pop/转场时触发
    void* navUserData = nullptr;

    // ---- SDK 控件行为钩子（继承重写）----
    /**
     * @brief 是否想成为原始 Pointer（Down/Move/Up/Cancel）输入目标，如 Button/ScrollView。
     */
    virtual bool acceptsPointerInput() const { return false; }
    /**
     * @brief 是否想成为滑动（pan）目标，如 ScrollView/Navigation。
     */
    virtual bool acceptsPanInput() const { return false; }
    /**
     * @brief 收到原始 Pointer 事件（Down/Move/Up/Cancel）；与 acceptsPointerInput
     * 配对：Button 用它跑按下态状态机、ScrollView 用它打断惯性动画。
     */
    virtual void handlePointer(const PointerEvent& /*event*/) {}
    /**
     * @brief 收到 pan 事件（BEGIN/UPDATE/END/CANCEL，坐标已换算为相对本视图）；
     * 与 acceptsPanInput 配对：ScrollView/Navigation 用它实现滚动/返回手势。
     */
    virtual void handlePan(const esx_view_pan_event& /*event*/) {}
    /**
     * @brief esx_view_set_bounds 之后调用（如 ScrollView 重 clamp、Navigation 重排）。
     */
    virtual void handleBoundsChanged() {}
    /**
     * @brief 子视图经 esx_destroy_view 移除前调用（index 为移除前在 children 中的
     * 下标），供 Flex 等容器同步与子视图平行的数据。
     */
    virtual void handleChildRemoved(size_t /*index*/) {}

    /**
     * @brief 挂载子视图，返回子视图裸指针（所有权归父视图）。
     * @param child 待挂载子视图（unique_ptr 所有权移交父视图）
     * @return 子视图裸指针（所有权归父视图，供调用方临时使用）
     */
    View* addChild(std::unique_ptr<View> child);

    /**
     * @brief 前序遍历重算 actual：actual = parent.actual + rect.x/y
     * （根视图 parent=null，actual=rect.x/y）。
     */
    void updateActuals();

    /**
     * @brief 命中测试：自身不可见或无命中返回 nullptr；从最上层（最后添加的）
     * child 往下递归，返回最深命中者。
     * @param px 屏幕坐标 x
     * @param py 屏幕坐标 y
     * @return 最深命中节点；无命中返回 nullptr
     */
    View* hitTest(float px, float py);

    /**
     * @brief 自身矩形（actual 坐标）。
     */
    Rect actualRect() const { return {actualX, actualY, rect.w, rect.h}; }

    /**
     * @brief 点是否同时位于自身及所有可见父视图内。
     * @param px 屏幕坐标 x
     * @param py 屏幕坐标 y
     */
    bool containsVisiblePoint(float px, float py) const;
};

} // namespace evk::ui
