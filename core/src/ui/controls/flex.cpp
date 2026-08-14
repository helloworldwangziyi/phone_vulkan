// ============================================================================
// Flex：线性布局容器（Flutter Column/Row 对应物）。
//
// 继承 View：specs 与 children 按下标一一对应（只经 esx_flex_set_child 挂
// 子视图，保证对齐）；layoutChildren 在挂载/spec 变更/尺寸变化时触发——
// 尺寸变化走 View::handleBoundsChanged 钩子，子视图被销毁走
// handleChildRemoved 同步 spec。
//
// 主轴分配：先扣固定项（main≥0）与全部 margin，剩余空间按 weight 比例
// 分给 weight>0 的项；weight=0 且 main<0 的项主轴尺寸为 0（规则见头文件）。
// ============================================================================

#include "evk/ui/controls/flex.h"

#include <algorithm>
#include <memory>

#include "evk/log.h"
#include "evk/ui/view.h"

namespace {

class FlexView : public evk::ui::View {
public:
    bool vertical = true;
    std::vector<esx_flex_child> specs; // 与 children 按下标一一对应

    const esx_flex_child& specOf(size_t index) const {
        static const esx_flex_child kDefault{-1.0f, 0.0f, -1.0f,
                                             ESX_FLEX_ALIGN_STRETCH, 0.0f, 0.0f, 0.0f};
        return index < specs.size() ? specs[index] : kDefault;
    }

    void layoutChildren() {
        const float mainSize = vertical ? rect.h : rect.w;
        const float crossSize = vertical ? rect.w : rect.h;
        if (mainSize <= 0.0f || crossSize <= 0.0f || children.empty()) {
            return;
        }

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

        float cursor = 0.0f;
        for (size_t i = 0; i < children.size(); ++i) {
            const esx_flex_child& s = specOf(i);
            const float childMain =
                s.main >= 0.0f
                    ? s.main
                    : (weightTotal > 0.0f ? remaining * s.weight / weightTotal : 0.0f);
            const float m = cursor + s.margin_main_before;

            float c = 0.0f;
            float childCross = 0.0f;
            if (s.cross >= 0.0f) {
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
                c = s.margin_cross;
                childCross = std::max(0.0f, crossSize - 2.0f * s.margin_cross);
            }

            const esx_view child = children[i]->handle;
            if (vertical) {
                esx_view_set_bounds(child, c, m, childCross, childMain);
            } else {
                esx_view_set_bounds(child, m, c, childMain, childCross);
            }
            cursor = m + childMain + s.margin_main_after;
        }
    }

    void handleBoundsChanged() override {
        layoutChildren();
    }

    void handleChildRemoved(size_t index) override {
        if (index < specs.size()) {
            specs.erase(specs.begin() + static_cast<std::ptrdiff_t>(index));
        }
        layoutChildren();
    }
};

FlexView* flexFromHandle(esx_view flex) {
    return dynamic_cast<FlexView*>(esxViewFromHandle(flex));
}

} // namespace

extern "C" {

esx_view esx_flex_create(float x, float y, float w, float h, esx_view parent,
                         int32_t vertical) {
    auto view = std::make_unique<FlexView>();
    view->vertical = vertical != 0;
    return esxAdoptViewNode(std::move(view), x, y, w, h, parent);
}

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
