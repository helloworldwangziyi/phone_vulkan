#include "evk/compositor.h"

#include "evk/frame_metrics.h"
#include "evk/frame_pipeline.h"
#include "evk/frame_scheduler.h"
#include "evk/log.h"
#include "evk/vulkan_renderer.h"
#include "evk/ui/render_view.h"
#include "ui/texture_store_source.h"

#include <atomic>
#include <stdexcept>

namespace evk {

struct Compositor::Impl {
    struct Frame {
        ui::Canvas canvas;
        std::vector<ui::TextureUpdate> textures;
        uint32_t width = 0, height = 0;
        uint64_t generation = 0;
        FrameTiming timing;
        FrameClock::time_point queued;
    };

    IPlatform* platform;
    // UI 独占；Raster 只读取 Frame 中的尺寸副本及原子 generation。
    uint32_t width = 0, height = 0;
    std::atomic<uint64_t> generation{1};
    bool firstFrame = true;
    // 以下两个对象只在 Raster 创建、使用和销毁。
    std::unique_ptr<ui::TextureStoreSource> textures;
    std::unique_ptr<Renderer> renderer;
    FramePipeline<Frame> pipeline;

    explicit Impl(IPlatform* value) : platform(value) {}
    ~Impl() { pipeline.stop(); }

    void raster(const Frame& frame) {
        FrameTiming timing = frame.timing;
        timing.milliseconds[static_cast<size_t>(FramePhase::Queue)] = frameMilliseconds(frame.queued);
        FrameTimingScope scope(timing);
        {
            FramePhaseScope rasterScope(FramePhase::Raster);
            // 过期几何可以跳过，纹理增量必须按序接收，不能丢 atlas 字形。
            textures->apply(frame.textures);
            if (frame.generation != generation.load()) return;
            if (rasterGeneration != frame.generation) {
                renderer->setSize(frame.width, frame.height);
                rasterGeneration = frame.generation;
            }
            const auto result = renderer->render(frame.canvas);
            if (result == RenderResult::Retry) {
                requestRender();
                return;
            }
            if (result == RenderResult::Failed) {
                EVK_LOGE("render", "raster_failed frame={} action=stop", timing.sequence);
                throw std::runtime_error("Raster rendering failed");
            }
        }
        FrameMetrics::instance().record(timing);
        if (++rendered % 120 == 1) {
            EVK_LOGD("render", "frame_timing frame={} ui_tasks_ms={:.2f} rebuild_ms={:.2f} layout_ms={:.2f} paint_ms={:.2f} snapshot_ms={:.2f} queue_ms={:.2f} raster_ms={:.2f} upload_ms={:.2f}",
                     timing.sequence, timing.ms(FramePhase::UiTasks), timing.ms(FramePhase::Rebuild),
                     timing.ms(FramePhase::Layout), timing.ms(FramePhase::Paint),
                     timing.ms(FramePhase::TextureSnapshot), timing.ms(FramePhase::Queue),
                     timing.ms(FramePhase::Raster), timing.ms(FramePhase::VertexUpload));
        }
    }
    uint64_t rasterGeneration = 0, rendered = 0;
};

Compositor::Compositor(IPlatform* platform) : impl_(std::make_unique<Impl>(platform)) {}
Compositor::~Compositor() = default;

bool Compositor::initialize() {
    auto& state = *impl_;
    // UIKit/JNI 平台属性只在调用线程查询，后续 resize 随帧下发。
    state.platform->getSurfaceSize(&state.width, &state.height);
    if (!state.width || !state.height) return false;
    const uint32_t width = state.width, height = state.height;
    return state.pipeline.start([&state, width, height] {
        state.textures = std::make_unique<ui::TextureStoreSource>();
        state.renderer = std::make_unique<Renderer>(state.platform, state.textures.get());
        state.renderer->setSize(width, height);
        state.rasterGeneration = state.generation.load();
        return state.renderer->initialize();
    }, [&state](const Impl::Frame& frame) { state.raster(frame); }, [&state] {
        state.renderer.reset();
        state.textures.reset();
    });
}

void Compositor::setSize(uint32_t width, uint32_t height) {
    impl_->width = width;
    impl_->height = height;
    requestSwapchainRebuild();
}

void Compositor::requestSwapchainRebuild() {
    ++impl_->generation;
    requestRender();
}

void Compositor::renderFrame() {
    auto& state = *impl_;
    if (!state.width || !state.height || !state.pipeline.running()) return;
    const bool submitted = state.pipeline.produce([&state](Impl::Frame& frame) {
        FrameTiming local;
        FrameTiming& timing = activeFrameTiming ? *activeFrameTiming : local;
        FrameTimingScope scope(timing);
        ui::buildFrame(frame.canvas);
        {
            FramePhaseScope snapshot(FramePhase::TextureSnapshot);
            frame.textures = ui::TextureStore::instance().takeUpdates(state.firstFrame);
        }
        frame.width = state.width;
        frame.height = state.height;
        frame.generation = state.generation.load();
        timing.vertices = frame.canvas.vertices().size();
        timing.batches = frame.canvas.batches().size();
        frame.timing = timing;
        frame.queued = FrameClock::now();
        if (state.firstFrame) {
            state.firstFrame = false;
            EVK_LOGI("render", "first_frame vertices={} batches={}", timing.vertices, timing.batches);
        }
    });
    if (!submitted) {
        FrameMetrics::instance().defer();
        requestRender(); // 保留脏请求，下个 VSync 用最新 UI 状态重试。
    }
}

} // namespace evk
