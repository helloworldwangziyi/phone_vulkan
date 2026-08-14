#pragma once

// 详情页（声明式）：渐变主视觉 + 2×2 色卡 + 继续 push 按钮。
// 层序号 variant 由构造传入（连续 push 时每层颜色不同，跳转有感知）。

#include <memory>

#include "evk/ui/widget.h"

class DetailPage : public evk::ui::Component {
public:
    explicit DetailPage(int variant) : variant_(variant) {}

    std::unique_ptr<evk::ui::Widget> build() override;

private:
    int variant_ = 0;
};
