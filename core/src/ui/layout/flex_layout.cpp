/**
 * @file flex_layout.cpp
 * @brief FlexView 的实现：两段式排布（先扣固定项与间距，剩余按 flex
 *        系数分配），交叉轴按对齐规则摆放。
 */

#include "evk/ui/layout/flex_layout.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "evk/ui/render_view.h"

namespace evk::ui {
namespace {

/**
 * @brief Flex 容器对应的 View：持有每个孩子的 FlexParentData，自身
 *        bounds 变化或孩子增删时全量重排。
 *
 * 没有增量布局与测量缓存：每棵子树尺寸变化都触发一次 O(n) 重排，
 * 靠「n 小 + 纯算术」保证便宜（对照 Flutter 的 RenderFlex 同理，
 * 只是它走完整的 constraints 协议）。
 */
class FlexView final : public View {
public:
    Axis axis = Axis::Vertical;
    std::vector<FlexParentData> parentData;

    const FlexParentData& dataAt(size_t index) const {
        static const FlexParentData fallback;
        return index < parentData.size() ? parentData[index] : fallback;
    }

    /**
     * 两段式排布：第一遍累加固定项（mainSize >= 0）与前/后间距，得出
     * 剩余主轴空间；第二遍定每个孩子的主轴尺寸（固定值或按 flex 瓜分
     * 剩余），再按交叉轴规则算偏移与尺寸，写 bounds（触发孩子级联）。
     */
    void layoutChildren() {
        const bool vertical = axis == Axis::Vertical;
        const float mainExtent = vertical ? rect.h : rect.w;
        const float crossExtent = vertical ? rect.w : rect.h;
        if (mainExtent <= 0.0f || crossExtent <= 0.0f) {
            return;
        }

        float fixed = 0.0f;
        float totalFlex = 0.0f;
        for (size_t i = 0; i < children.size(); ++i) {
            const FlexParentData& data = dataAt(i);
            fixed += data.before + data.after;
            if (data.mainSize >= 0.0f) {
                fixed += data.mainSize;
            } else {
                totalFlex += std::max(0.0f, data.flex);
            }
        }
        const float remaining = std::max(0.0f, mainExtent - fixed);

        float cursor = 0.0f;
        for (size_t i = 0; i < children.size(); ++i) {
            const FlexParentData& data = dataAt(i);
            const float childMain = data.mainSize >= 0.0f
                ? data.mainSize
                : totalFlex > 0.0f
                    ? remaining * std::max(0.0f, data.flex) / totalFlex
                    : 0.0f;
            const float mainOffset = cursor + data.before;

            float crossOffset = data.crossMargin;
            float childCross = data.crossSize;
            if (childCross < 0.0f ||
                data.crossAlignment == CrossAxisAlignment::Stretch) {
                childCross = std::max(0.0f, crossExtent - 2.0f * data.crossMargin);
            } else {
                switch (data.crossAlignment) {
                    case CrossAxisAlignment::Start:
                    case CrossAxisAlignment::Stretch:
                        crossOffset = data.crossMargin;
                        break;
                    case CrossAxisAlignment::Center:
                        crossOffset = (crossExtent - childCross) * 0.5f;
                        break;
                    case CrossAxisAlignment::End:
                        crossOffset = crossExtent - data.crossMargin - childCross;
                        break;
                }
            }

            if (vertical) {
                children[i]->setBounds(crossOffset, mainOffset, childCross, childMain);
            } else {
                children[i]->setBounds(mainOffset, crossOffset, childMain, childCross);
            }
            cursor = mainOffset + childMain + data.after;
        }
    }

    void handleBoundsChanged() override {
        layoutChildren();
    }

    void handleChildRemoved(size_t index) override {
        if (index < parentData.size()) {
            parentData.erase(parentData.begin() + static_cast<std::ptrdiff_t>(index));
        }
        layoutChildren();
    }
};

FlexView* asFlex(View& view) {
    return dynamic_cast<FlexView*>(&view);
}

} // namespace

std::unique_ptr<View> createFlexView(Axis axis) {
    auto flex = std::make_unique<FlexView>();
    flex->axis = axis;
    return flex;
}

void setFlexChild(View& flex, View& child, const FlexParentData& data) {
    FlexView* container = asFlex(flex);
    if (!container || child.parent != container) {
        return;
    }
    for (size_t i = 0; i < container->children.size(); ++i) {
        if (container->children[i].get() == &child) {
            if (container->parentData.size() <= i) {
                container->parentData.resize(i + 1);
            }
            container->parentData[i] = data;
            container->layoutChildren();
            return;
        }
    }
}

} // namespace evk::ui
