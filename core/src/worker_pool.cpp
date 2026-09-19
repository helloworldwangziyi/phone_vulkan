#include "evk/worker_pool.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace evk {

struct WorkerPool::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    size_t maxPending;
    bool stopping = false;

    explicit Impl(size_t limit) : maxPending(std::max(size_t{1}, limit)) {}
    void stop() {
        std::deque<std::function<void()>> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            discarded.swap(jobs);
        }
        wake.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~Impl() { stop(); }
};

WorkerPool::WorkerPool(size_t threads, size_t maxPending)
    : impl_(std::make_unique<Impl>(maxPending)) {
    if (threads == 0) threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    // 构造中途线程创建失败时 Impl 的析构仍会唤醒、join 已创建线程。
    for (size_t i = 0; i < threads; ++i) {
        impl_->threads.emplace_back([state = impl_.get()] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->wake.wait(lock, [state] { return state->stopping || !state->jobs.empty(); });
                    if (state->stopping) return;
                    job = std::move(state->jobs.front());
                    state->jobs.pop_front();
                }
                job();
            }
        });
    }
}

WorkerPool::~WorkerPool() {
    alive_->store(false);
    impl_->stop();
}

WorkerPool& WorkerPool::instance() {
    static WorkerPool pool;
    return pool;
}

bool WorkerPool::enqueue(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopping || impl_->jobs.size() >= impl_->maxPending) return false;
        impl_->jobs.push_back(std::move(job));
    }
    impl_->wake.notify_one();
    return true;
}

} // namespace evk
