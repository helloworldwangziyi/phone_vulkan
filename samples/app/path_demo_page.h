#pragma once

#include <memory>

#include "evk/ui/widget_tree.h"

/// 矢量路径（Path）演示页：展示贝塞尔曲线填充/描边、凹多边形三角化。
class PathDemoPage final : public evk::ui::StatelessWidget {
public:
    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext& context) const override;
};
