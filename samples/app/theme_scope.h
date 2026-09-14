#pragma once

/// @file theme_scope.h
/// ============================================================================
/// 树内依赖下发（InheritedWidget）演示：局部主题作用域。
///
/// 与 app_theme.h 的 EventBus 广播换肤对照：
///   - EventBus 路径：换肤 → 广播 → 各页面 listen 后 setState 全量重建；
///   - ThemeScope 路径：色值放在 InheritedWidget 上沿树下发，子孙在 build()
///     里 ThemeScope::of(context) 读取并登记依赖，数据变更时依赖它的子树
///     被精确重建——不用手工订阅，也不惊扰未登记的兄弟分支。
/// ============================================================================

#include <stdint.h>

#include "evk/ui/widgets.h"

/// 深/浅两套色值的局部主题作用域。演示见 HomePage 的 InheritedThemeDemo。
class ThemeScope : public evk::ui::InheritedWidget {
public:
    ThemeScope(bool dark, std::unique_ptr<evk::ui::Widget> child)
        : InheritedWidget(std::move(child)), dark_(dark) {}

    bool dark() const { return dark_; }
    uint32_t boxColor() const { return dark_ ? 0x2A2F3AFF : 0xF2C94CFF; }
    uint32_t textColor() const { return dark_ ? 0xF5F5F5FF : 0x1C1F26FF; }

    bool updateShouldNotify(const InheritedWidget& oldWidget) const override {
        return dark_ != static_cast<const ThemeScope&>(oldWidget).dark_;
    }

    /// 读取最近作用域并登记依赖（build() 里调用；树上必须挂了 ThemeScope）。
    static const ThemeScope& of(evk::ui::BuildContext& context) {
        return *context.dependOnInheritedWidgetOfExactType<ThemeScope>();
    }

private:
    bool dark_;
};
