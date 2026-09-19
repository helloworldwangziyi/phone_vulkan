#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace evk {

using FrameClock = std::chrono::steady_clock;
inline double frameMilliseconds(FrameClock::time_point since) {
    return std::chrono::duration<double, std::milli>(FrameClock::now() - since).count();
}

enum class FramePhase { UiTasks, Animation, Rebuild, Layout, Paint, TextureSnapshot,
                        Queue, Raster, VertexUpload, Count };
struct FrameTiming {
    uint64_t sequence = 0;
    std::array<double, static_cast<size_t>(FramePhase::Count)> milliseconds{};
    size_t vertices = 0, batches = 0;
    double ms(FramePhase phase) const { return milliseconds[static_cast<size_t>(phase)]; }
};

// 记录 CPU 耗时，Raster 含 fence/acquire/present 等待，不等同于 GPU 执行时间。
// 阶段有包含关系（如 UiTasks 内同步 Rebuild），不可简单相加。
inline thread_local FrameTiming* activeFrameTiming = nullptr;
inline thread_local std::array<unsigned, static_cast<size_t>(FramePhase::Count)> framePhaseDepth{};
class FrameTimingScope {
public:
    explicit FrameTimingScope(FrameTiming& timing) : previous_(activeFrameTiming) {
        activeFrameTiming = &timing;
    }
    ~FrameTimingScope() { activeFrameTiming = previous_; }
private:
    FrameTiming* previous_;
};
class FramePhaseScope {
public:
    explicit FramePhaseScope(FramePhase phase)
        : phase_(static_cast<size_t>(phase)), timing_(activeFrameTiming) {
        if (timing_ && framePhaseDepth[phase_]++ == 0) start_ = FrameClock::now();
    }
    ~FramePhaseScope() {
        if (timing_ && --framePhaseDepth[phase_] == 0)
            timing_->milliseconds[phase_] += frameMilliseconds(start_);
    }
private:
    size_t phase_;
    FrameTiming* timing_;
    FrameClock::time_point start_;
};

struct FrameMetricsSnapshot {
    FrameTiming latest;
    uint64_t rendered = 0, deferred = 0;
};
class FrameMetrics {
public:
    static FrameMetrics& instance() { static FrameMetrics metrics; return metrics; }
    void record(const FrameTiming& timing) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.latest = timing;
        ++snapshot_.rendered;
    }
    void defer() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++snapshot_.deferred;
    }
    FrameMetricsSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }
private:
    mutable std::mutex mutex_;
    FrameMetricsSnapshot snapshot_;
};

} // namespace evk
