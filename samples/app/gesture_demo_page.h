#pragma once

#include <memory>

#include "evk/ui/widget_tree.h"

/// 手势演示页：tap / double tap / long press / scale（捏合+旋转）可视化。
class GestureDemoPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};
