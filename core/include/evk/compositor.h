#pragma once

/**
 * @file compositor.h
 * @brief 帧编排器：buildFrame（视图树 → Canvas）→ Renderer::render 的串接点。
 */
#include <cstdint>
#include <memory>

namespace evk {

class IPlatform;

/**
 * @brief Compositor：把"每帧重建 Canvas 并提交渲染器"这段帧编排内聚到 core，
 * 平台桥（Android/iOS/鸿蒙）只剩生命周期接线。
 *
 * UI 独占视图树并产出 Canvas/纹理脏区快照；两个帧槽交给专用 Raster
 * 线程，Renderer 的创建、录制、上传、提交和销毁全部在 Raster。
 * 公开方法在 UI 串行调用；析构 join 后平台才能释放窗口。
 */
class Compositor {
public:
    /**
     * @brief 创建 Compositor；initialize 时启动 Raster 并创建 Renderer。
     * @param platform 平台抽象层（不持有所有权）
     */
    explicit Compositor(IPlatform* platform);
    ~Compositor();

    /**
     * @brief 初始化内部渲染器的整套 Vulkan 管线。
     * @return true 表示成功；失败则由调用方销毁本对象
     */
    bool initialize();

    /// UI 线程调用，尺寸/重建请求随帧下发，不能跨线程直接访问 Renderer。
    void setSize(uint32_t width, uint32_t height);
    void requestSwapchainRebuild();

    /**
     * @brief 画一帧：buildFrame 收集本帧几何（内部执行 View draw callback），
     * 满队列时保留下一帧请求并立即返回，不等待 GPU。
     */
    void renderFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace evk
