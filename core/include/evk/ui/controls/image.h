#pragma once

/**
 * @file image.h
 * @brief 位图组件（ImageWidget）：把 TextureStore 纹理拉伸铺满自身边界。
 *
 * 对照 Flutter 的 widgets/image.dart（无网络加载、无渐显的简化版）；
 * 纹理由 TextureStore 预登记，Widget 只持句柄。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 位图组件：把 TextureStore 纹理拉伸铺满自身边界。
 *
 * 无固有尺寸——在容器里跟随拉伸（配合 SizedBox/Expanded 控制大小）；
 * 顶点色恒白即原样贴图。要染色/九宫格时用 Container::painter 自绘。
 */
class ImageWidget final : public RenderObjectWidget {
public:
    explicit ImageWidget(TextureId texture);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;

private:
    TextureId texture_; ///< TextureStore 句柄
};

/// 构造辅助：一行造一个位图。
std::unique_ptr<Widget> image(TextureId texture);

} // namespace evk::ui
