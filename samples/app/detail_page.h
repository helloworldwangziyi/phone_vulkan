#pragma once

#include <memory>

#include "evk/ui/widget_tree.h"

class DetailPage final : public evk::ui::StatelessWidget {
public:
    explicit DetailPage(int variant) : variant_(variant) {}

    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext& context) const override;

private:
    int variant_ = 0;
};
