/**
 * @file compositor.cpp
 * @brief 帧编排器实现：buildFrame → 首帧日志 → renderer->render。
 */
#include "evk/compositor.h"

// EVK_LOGI 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"
#include "evk/vulkan_renderer.h"
// ui::buildFrame：视图树 → Canvas 的帧构建入口。
#include "evk/ui/render_view.h"
// TextureStore → gpu::ITextureSource 的 header-only 适配器（UI 层提供）。
#include "ui/texture_store_source.h"

namespace evk {

Compositor::Compositor(IPlatform* platform)
    : textureSource_(std::make_unique<ui::TextureStoreSource>()),
      renderer_(std::make_unique<Renderer>(platform, textureSource_.get())) {}

// 析构定义在实现文件：成员是前置声明类型的 unique_ptr，此处类型才完整。
Compositor::~Compositor() = default;

bool Compositor::initialize() {
    return renderer_->initialize();
}

Renderer* Compositor::renderer() const {
    return renderer_.get();
}

void Compositor::renderFrame() {
    // 构建视图树内容（内部执行 View draw callback）后交给渲染器；
    // 首帧打一次顶点/批次统计日志。
    ui::buildFrame(canvas_);
    if (!firstFrameLogged_) {
        firstFrameLogged_ = true;
        EVK_LOGI("first UI frame: vertices={}, batches={}",
                 canvas_.vertices().size(), canvas_.batches().size());
    }
    renderer_->render(canvas_);
}

} // namespace evk
