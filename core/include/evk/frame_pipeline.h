#pragma once

#include <array>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace evk {

// 单 UI 生产者、单 Raster 消费者。两个槽包含正在消费的帧；槽内存循环复用。
// produce/start/stop 由 UI 串行调用；满时 produce 不调用 builder，也不等待 GPU。
// stop 丢弃尚未消费的帧，等待当前消费与 finish 完成，之后才能释放原生窗口。
template <typename Frame>
class FramePipeline {
public:
    ~FramePipeline() { stop(); }
    FramePipeline() = default;
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    bool start(std::function<bool()> initialize,
               std::function<void(const Frame&)> consume,
               std::function<void()> finish) {
        if (started_) return false;
        started_ = true;
        std::promise<bool> ready;
        auto initialized = ready.get_future();
        thread_ = std::thread([this, initialize = std::move(initialize),
                               consume = std::move(consume), finish = std::move(finish),
                               ready = std::move(ready)]() mutable {
            bool ok = false;
            try { ok = initialize(); }
            catch (...) { recordFailure(); }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = ok;
            }
            ready.set_value(ok);
            if (ok) {
                try {
                    for (;;) {
                        size_t index;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            wake_.wait(lock, [this] { return stopping_ || queued_ > 0; });
                            if (stopping_) break;
                            index = queue_[head_];
                            head_ = (head_ + 1) % queue_.size();
                            --queued_;
                        }
                        consume(frames_[index]);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            busy_[index] = false;
                        }
                    }
                } catch (...) { recordFailure(); }
            }
            try { finish(); }
            catch (...) { recordFailure(); }
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        });
        if (initialized.get()) return true;
        stop();
        return false;
    }

    template <typename Builder>
    bool produce(Builder&& builder) {
        size_t index = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_) return false;
            while (index < busy_.size() && busy_[index]) ++index;
            if (index == busy_.size()) return false;
            busy_[index] = true;
        }
        try { builder(frames_[index]); }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_[index] = false;
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_[(head_ + queued_) % queue_.size()] = index;
            ++queued_;
        }
        wake_.notify_one();
        return true;
    }

    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && !stopping_;
    }

    std::exception_ptr failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void recordFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = std::current_exception();
        stopping_ = true;
    }
    std::array<Frame, 2> frames_;
    std::array<bool, 2> busy_{};
    std::array<size_t, 2> queue_{};
    size_t head_ = 0, queued_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool running_ = false, stopping_ = false, started_ = false;
    std::exception_ptr failure_;
};

} // namespace evk
