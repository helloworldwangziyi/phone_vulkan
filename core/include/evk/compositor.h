#pragma once

/**
 * @file compositor.h
 * @brief 帧编排器：buildFrame（视图树 → Canvas）→ Renderer::render 的串接点。
 */
#include "evk/ui/paint_canvas.h"
#include <memory>

namespace evk {

class IPlatform;
class Renderer;
namespace gpu {
class ITextureSource;
}

/**
 * @brief Compositor：把"每帧重建 Canvas 并提交渲染器"这段帧编排内聚到 core，
 * 平台桥（Android/iOS）只剩生命周期接线。
 *
 * 持有 Renderer 与帧 Canvas，并装配 TextureStore → gpu::ITextureSource
 * 的适配器；注册为 core FrameFunc 的内容就是 renderFrame()。
 */
class Compositor {
public:
    /**
     * @brief 创建 Compositor（内部创建 Renderer，并注入 UI 纹理源适配器）。
     * @param platform 平台抽象层（不持有所有权）
     */
    explicit Compositor(IPlatform* platform);
    ~Compositor();

    /**
     * @brief 初始化内部渲染器的整套 Vulkan 管线。
     * @return true 表示成功；失败则由调用方销毁本对象
     */
    bool initialize();

    /// 内部渲染器（setSize / requestSwapchainRebuild 等平台事件直达）。
    Renderer* renderer() const;

    /**
     * @brief 画一帧：buildFrame 收集本帧几何（内部执行 View draw callback），
     * 首帧记录一次顶点/批次统计日志，然后交给渲染器提交。
     */
    void renderFrame();

private:
    std::unique_ptr<gpu::ITextureSource> textureSource_; ///< TextureStore 适配器
    std::unique_ptr<Renderer> renderer_;
    ui::Canvas canvas_; ///< 帧 Canvas：每帧 clear 后从零重建（见 paint_canvas.h）
    /// 首帧统计日志只打一次（随实例生命周期复位，surface 重建后重新打）。
    bool firstFrameLogged_ = false;
};

} // namespace evk
