#pragma once

#include <memory>

#include "evk/ui/widget_tree.h"

/// 渲染特效演示页：阴影（BoxShadow）/ 圆角裁剪（ClipRRect）/ 背景模糊
/// （BackdropBlur）三项新渲染能力的可视化验证。
class EffectsDemoPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};
