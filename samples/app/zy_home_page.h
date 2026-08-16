#pragma once

#include <memory>

#include "evk/ui/zy_widget_tree.h"

class HomePage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};
