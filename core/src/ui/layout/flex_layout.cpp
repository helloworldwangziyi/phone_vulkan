/**
 * @file flex_layout.cpp
 * @brief FlexView 的实现：约束协议下的两段式排布——先测量非弹性孩子
 *        （约束下行、尺寸上行），再把剩余主轴空间按 flex 系数分给弹性项。
 */

#include "evk/ui/layout/flex_layout.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "evk/ui/render_view.h"

namespace evk::ui {
namespace {

/**
 * @brief Flex 容器对应的 View：持有每个孩子的 FlexParentData（布局期
 *        parent data），performLayout 里执行两段式排布。
 *
 * 对照 Flutter 的 RenderFlex：
 *   - 第一遍（测量）：非弹性孩子收「主轴松散 + 交叉轴按对齐」的约束
 *     自报尺寸（内容自适应；显式 mainSize/crossSize 是 tight 覆盖）；
 *   - 第二遍（分配）：主轴有界时，剩余空间按 flex 系数 tight 分给弹性
 *     孩子（Expanded）；主轴无界遇弹性孩子收敛为 0（Flutter 在此报错）。
 * 自身回报：主轴有界 → 撑满，无界 → 孩子加总；交叉轴同理（有界撑满、
 * 无界取孩子最大 footprint）——嵌套 Column/Row 的固有尺寸由此而来。
 */
class FlexView final : public View {
public:
    Axis axis = Axis::Vertical;
    std::vector<FlexParentData> parentData;

    const FlexParentData& dataAt(size_t index) const {
        static const FlexParentData fallback;
        return index < parentData.size() ? parentData[index] : fallback;
    }

    Size performLayout(const BoxConstraints& constraints) override {
        const bool vertical = axis == Axis::Vertical;
        const float maxMain = vertical ? constraints.maxHeight : constraints.maxWidth;
        const float maxCross = vertical ? constraints.maxWidth : constraints.maxHeight;
        const bool mainBounded =
            vertical ? constraints.isHeightBounded() : constraints.isWidthBounded();
        const bool crossBounded =
            vertical ? constraints.isWidthBounded() : constraints.isHeightBounded();

        const size_t count = children.size();
        std::vector<float> mainSizes(count, 0.0f);
        std::vector<float> crossSizes(count, 0.0f);

        // 第一遍：固定/自测孩子布局，累计主轴占用、flex 总和与最大交叉尺寸。
        float fixed = 0.0f;
        float totalFlex = 0.0f;
        float maxCrossFootprint = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const FlexParentData& data = dataAt(i);
            fixed += data.before + data.after;
            if (data.mainSize < 0.0f && data.flex > 0.0f) {
                totalFlex += std::max(0.0f, data.flex);
                continue; // 弹性项第二遍分配
            }
            const Size size = children[i]->layout(childConstraints(data));
            mainSizes[i] = vertical ? size.height : size.width;
            crossSizes[i] = vertical ? size.width : size.height;
            fixed += mainSizes[i];
            maxCrossFootprint = std::max(
                maxCrossFootprint, crossSizes[i] + 2.0f * data.crossMargin);
        }

        // 第二遍：弹性孩子按 flex 瓜分剩余主轴空间（主轴无界时分 0）。
        const float remaining =
            mainBounded ? std::max(0.0f, maxMain - fixed) : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const FlexParentData& data = dataAt(i);
            if (data.mainSize >= 0.0f || data.flex <= 0.0f) {
                continue;
            }
            const float share =
                totalFlex > 0.0f ? remaining * std::max(0.0f, data.flex) / totalFlex
                                 : 0.0f;
            const Size size = children[i]->layout(childConstraints(data, share));
            mainSizes[i] = vertical ? size.height : size.width;
            crossSizes[i] = vertical ? size.width : size.height;
            maxCrossFootprint = std::max(
                maxCrossFootprint, crossSizes[i] + 2.0f * data.crossMargin);
        }

        // 定位：主轴顺序排，交叉轴按对齐规则算偏移（孩子尺寸已上行）。
        // 拉伸（Stretch 且无显式 crossSize）的孩子已 tight 铺满交叉轴，
        // 偏移即 crossMargin；其余孩子按 Start/Center/End 在交叉轴内摆放
        // （参照系：有界取约束上限，无界取孩子最大 footprint）。
        const float crossExtent = crossBounded ? maxCross : maxCrossFootprint;
        float cursor = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const FlexParentData& data = dataAt(i);
            const float mainOffset = cursor + data.before;
            const bool stretched = data.crossSize < 0.0f &&
                data.crossAlignment == CrossAxisAlignment::Stretch &&
                crossBounded;
            float crossOffset = data.crossMargin;
            if (!stretched) {
                switch (data.crossAlignment) {
                    case CrossAxisAlignment::Center:
                        crossOffset = (crossExtent - crossSizes[i]) * 0.5f;
                        break;
                    case CrossAxisAlignment::End:
                        crossOffset =
                            crossExtent - data.crossMargin - crossSizes[i];
                        break;
                    case CrossAxisAlignment::Start:
                    case CrossAxisAlignment::Stretch:
                        break; // 无界下的 Stretch 已退化为松散自测 → 按 Start
                }
            }
            if (vertical) {
                children[i]->setPosition(crossOffset, mainOffset);
            } else {
                children[i]->setPosition(mainOffset, crossOffset);
            }
            cursor = mainOffset + mainSizes[i] + data.after;
        }

        // 自报尺寸：有界撑满，无界包内容。
        const float myMain = mainBounded ? maxMain : cursor;
        const float myCross = crossBounded ? maxCross : maxCrossFootprint;
        return vertical ? Size{myCross, myMain} : Size{myMain, myCross};
    }

    /**
     * 孩子的下行约束。主轴：显式 mainSize / flex 份额给 tight，否则**无界**
     * 让孩子自测（对照 Flutter：Column 给非弹性孩子无限高；裸叶子在无界
     * 轴取 0，与旧「无尺寸=0」表现一致，Text 则报实测行高）。交叉轴：
     * 显式 crossSize 或 Stretch 给 tight（扣 crossMargin，无界时退化为
     * 松散），其余对齐给松散——孩子尺寸上行后由定位阶段计算偏移。
     */
    BoxConstraints childConstraints(const FlexParentData& data,
                                    float tightMain = -1.0f) const {
        const bool vertical = axis == Axis::Vertical;
        const float maxCross = vertical ? constraints().maxWidth
                                        : constraints().maxHeight;
        const bool crossBounded = vertical ? constraints().isWidthBounded()
                                           : constraints().isHeightBounded();
        const float crossLimit =
            crossBounded ? std::max(0.0f, maxCross - 2.0f * data.crossMargin)
                         : BoxConstraints::kInfinite;

        float minMain = 0.0f;
        float maxMainLimit = BoxConstraints::kInfinite;
        if (tightMain >= 0.0f) {
            minMain = maxMainLimit = tightMain;
        } else if (data.mainSize >= 0.0f) {
            minMain = maxMainLimit = data.mainSize;
        }

        float minCross = 0.0f;
        float maxCrossLimit = crossLimit;
        if (data.crossSize >= 0.0f) {
            minCross = maxCrossLimit = data.crossSize;
        } else if (data.crossAlignment == CrossAxisAlignment::Stretch &&
                   crossBounded) {
            minCross = crossLimit;
        }

        return vertical
            ? BoxConstraints{minCross, maxCrossLimit, minMain, maxMainLimit}
            : BoxConstraints{minMain, maxMainLimit, minCross, maxCrossLimit};
    }

    void handleChildRemoved(size_t index) override {
        if (index < parentData.size()) {
            parentData.erase(parentData.begin() + static_cast<std::ptrdiff_t>(index));
        }
        // 重排由基类 removeChild 的 markNeedsLayout + flushLayout 覆盖。
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
            container->markNeedsLayout();
            container->flushLayout();
            return;
        }
    }
}

} // namespace evk::ui
