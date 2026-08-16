#pragma once

#include <cstdint>
#include <memory>

namespace evk::ui {

class View;

enum class Axis {
    Horizontal,
    Vertical,
};

enum class CrossAxisAlignment {
    Stretch,
    Start,
    Center,
    End,
};

struct FlexParentData {
    float mainSize = -1.0f;
    float flex = 0.0f;
    float crossSize = -1.0f;
    CrossAxisAlignment crossAlignment = CrossAxisAlignment::Stretch;
    float before = 0.0f;
    float after = 0.0f;
    float crossMargin = 0.0f;
};

std::unique_ptr<View> createFlexView(Axis axis);
void setFlexChild(View& flex, View& child, const FlexParentData& data);

} // namespace evk::ui
