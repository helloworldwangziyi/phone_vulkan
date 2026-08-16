#pragma once

#include <functional>
#include <memory>

namespace evk::ui {

class View;

std::unique_ptr<View> createScrollView(
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});

View* scrollContent(View& scrollView);
void updateScrollView(
    View& scrollView,
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});
void setScrollOffset(View& scrollView, float x, float y);
void getScrollOffset(const View& scrollView, float* x, float* y);

} // namespace evk::ui
