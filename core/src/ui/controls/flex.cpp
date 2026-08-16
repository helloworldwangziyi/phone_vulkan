/**
 * @file flex.cpp
 * @brief Flex：线性布局容器（Flutter Column/Row 对应物）。
 *
 * 继承 View：specs 与 children 按下标一一对应（只经 esx_flex_set_child 挂
 * 子视图，保证对齐）；layoutChildren 在挂载/spec 变更/尺寸变化时触发——
 * 尺寸变化走 View::handleBoundsChanged 钩子，子视图被销毁走
 * handleChildRemoved 同步 spec。
 *
 * 主轴分配：先扣固定项（main≥0）与全部 margin，剩余空间按 weight 比例
 * 分给 weight>0 的项；weight=0 且 main<0 的项主轴尺寸为 0（规则见头文件）。
 */

#include "evk/ui/controls/flex.h"

#include <algorithm>
#include <memory>

#include "evk/log.h"
#include "evk/ui/view.h"

namespace {

/**
 * @brief Flex 容器实现：specs 与 children 按下标一一对应（平行数组）。
 */
class FlexView : public evk::ui::View {
public:
    bool vertical = true; ///< true=Column（主轴纵），false=Row（主轴横）
    /**
     * @brief 排布参数表：与父类 children 按下标一一对应（平行数组）。
     *
     * 子视图可能经 esxAdoptChild 直接挂入（声明式 reconcile 路径），
     * 此时 specs 会补齐对齐；子视图销毁时 handleChildRemoved 同步删除。
     */
    std::vector<esx_flex_child> specs;

    /// 取第 index 个子项的排布参数；无对应项时返回默认 spec。
    const esx_flex_child& specOf(size_t index) const {
        static const esx_flex_child kDefault{-1.0f, 0.0f, -1.0f,
                                             ESX_FLEX_ALIGN_STRETCH, 0.0f, 0.0f, 0.0f};
        return index < specs.size() ? specs[index] : kDefault;
    }

    /**
     * @brief Flex 布局核心算法（线性布局，两趟完成）。
     *
     * 第一趟统计：主轴先扣掉所有固定项（main≥0）与全部 margin，
     *   得到剩余空间 remaining，并累计 weight 总和；
     * 第二趟逐个摆放：固定项取 main；weight>0 的项按比例瓜分 remaining
     *   （即声明式层的 Expanded/flex(1) 语义）；weight=0 且 main<0 的项主轴为 0。
     * 交叉轴：cross≥0 为固定尺寸并按 align（START/END/CENTER）对齐（带 margin），
     *   cross<0 则 stretch 撑满（减去两侧 margin_cross）。
     * 结果直接 esx_view_set_bounds 写进子视图 rect——这会触发子视图的
     * handleBoundsChanged，把布局自上而下多米诺式传播到整棵子树。
     * 触发时机（三种入口最终都收敛到这里）：挂载（esx_flex_set_child）、
     *   spec 变更、自身尺寸变化（handleBoundsChanged）。
     */
    void layoutChildren() {
        const float mainSize = vertical ? rect.h : rect.w;
        const float crossSize = vertical ? rect.w : rect.h;
        if (mainSize <= 0.0f || crossSize <= 0.0f || children.empty()) {
            return; // 尺寸无效或无子节点：无布局可言（创建初期 rect 全是 0）
        }

        // ---- 第一趟：统计固定项总占与 weight 总和 ----
        float fixedTotal = 0.0f;
        float weightTotal = 0.0f;
        for (size_t i = 0; i < children.size(); ++i) {
            const esx_flex_child& s = specOf(i);
            fixedTotal += s.margin_main_before + s.margin_main_after;
            if (s.main >= 0.0f) {
                fixedTotal += s.main;
            } else {
                weightTotal += s.weight;
            }
        }
        const float remaining = std::max(0.0f, mainSize - fixedTotal);

        // ---- 第二趟：逐个计算子视图 rect 并写回 ----
        float cursor = 0.0f; // 主轴游标：下一个子项的起始位置（未含其前导 margin）
        for (size_t i = 0; i < children.size(); ++i) {
            const esx_flex_child& s = specOf(i);
            // 主轴尺寸：固定项直接取 main；弹性项按 weight 瓜分剩余空间。
            const float childMain =
                s.main >= 0.0f
                    ? s.main
                    : (weightTotal > 0.0f ? remaining * s.weight / weightTotal : 0.0f);
            // 主轴起点 = 游标 + 本项前导 margin。
            const float m = cursor + s.margin_main_before;

            // 交叉轴：c = 交叉轴起点，childCross = 交叉轴尺寸。
            float c = 0.0f;
            float childCross = 0.0f;
            if (s.cross >= 0.0f) {
                // 固定交叉轴尺寸：按 align 对齐（START 贴前 / END 贴后 / CENTER 居中）。
                childCross = s.cross;
                switch (s.align) {
                    case ESX_FLEX_ALIGN_START:
                        c = s.margin_cross;
                        break;
                    case ESX_FLEX_ALIGN_END:
                        c = crossSize - s.margin_cross - childCross;
                        break;
                    case ESX_FLEX_ALIGN_CENTER:
                    default:
                        c = (crossSize - childCross) * 0.5f;
                        break;
                }
            } else {
                // stretch：撑满交叉轴（cross<0 的默认行为），两侧留 margin_cross。
                c = s.margin_cross;
                childCross = std::max(0.0f, crossSize - 2.0f * s.margin_cross);
            }

            // 把算好的矩形写进子视图（vertical 时 x=交叉轴 y=主轴，横向反之）。
            // 注意 esx_view_set_bounds 会触发子视图的 handleBoundsChanged，
            // 布局由此继续向更深层级联。
            const esx_view child = children[i]->handle;
            if (vertical) {
                esx_view_set_bounds(child, c, m, childCross, childMain);
            } else {
                esx_view_set_bounds(child, m, c, childMain, childCross);
            }
            cursor = m + childMain + s.margin_main_after;
        }
    }

    /// 自身尺寸被 esx_view_set_bounds 修改后，重排所有子节点（布局级联的一环）。
    void handleBoundsChanged() override {
        layoutChildren();
    }

    /**
     * @brief 子视图被销毁时同步删除与之平行的 spec（specs 与 children 按下标
     * 一一对应），否则后续 layoutChildren 会拿错参数或越界。
     */
    void handleChildRemoved(size_t index) override {
        if (index < specs.size()) {
            specs.erase(specs.begin() + static_cast<std::ptrdiff_t>(index));
        }
        layoutChildren();
    }
};

/// 句柄 → FlexView；句柄无效或视图不是 Flex 时返回 nullptr。
FlexView* flexFromHandle(esx_view flex) {
    return dynamic_cast<FlexView*>(esxViewFromHandle(flex));
}

} // namespace

extern "C" {

esx_view esx_flex_create(esx_view parent, int32_t vertical) {
    auto view = std::make_unique<FlexView>();
    view->vertical = vertical != 0;
    return esxAdoptViewNode(std::move(view), parent);
}

/**
 * @brief 设置/更新子视图的排布参数（声明式层 configureChild 的落点）。
 *
 * 两条路径：子视图已在 children 里（可能经 reconcile 直接 adopt）→ 按下标
 * 更新 spec；子视图未挂载（parent=0）→ 先 adopt 进容器再追加 spec。
 * 每次 spec 落地都会 layoutChildren 全量重排（当前规模下全量重排足够便宜）。
 */
void esx_flex_set_child(esx_view flex, esx_view child, const esx_flex_child* spec) {
    FlexView* self = flexFromHandle(flex);
    if (!self) {
        EVK_LOGW("esx_flex_set_child: flex {} is not a Flex view", flex);
        return;
    }
    const esx_flex_child value = spec ? *spec : esx_flex_child{-1.0f, 0.0f, -1.0f,
                                                               ESX_FLEX_ALIGN_STRETCH,
                                                               0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < self->children.size(); ++i) {
        if (self->children[i]->handle == child) {
            // 子视图可能已被上层（如 reconcile）直接 adopt：specs 补齐对齐。
            if (self->specs.size() <= i) {
                self->specs.resize(i + 1, esx_flex_child{-1.0f, 0.0f, -1.0f,
                                                         ESX_FLEX_ALIGN_STRETCH,
                                                         0.0f, 0.0f, 0.0f});
            }
            self->specs[i] = value;
            self->layoutChildren();
            return;
        }
    }
    evk::ui::View* childView = esxViewFromHandle(child);
    if (!childView) {
        EVK_LOGW("esx_flex_set_child: unknown child handle {}", child);
        return;
    }
    if (childView->parent != nullptr || esxRootView() == childView) {
        EVK_LOGW("esx_flex_set_child: child {} must be created with parent=0", child);
        return;
    }
    if (!esxAdoptChild(flex, child)) {
        return;
    }
    self->specs.push_back(value);
    self->layoutChildren();
}

} // extern "C"
