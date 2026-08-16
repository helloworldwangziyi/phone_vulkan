#include "evk/ui/layout/zy_flex_layout.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "evk/ui/zy_render_view.h"

namespace evk::ui {
namespace {

class FlexView final : public View {
public:
    Axis axis = Axis::Vertical;
    std::vector<FlexParentData> parentData;

    const FlexParentData& dataAt(size_t index) const {
        static const FlexParentData fallback;
        return index < parentData.size() ? parentData[index] : fallback;
    }

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
