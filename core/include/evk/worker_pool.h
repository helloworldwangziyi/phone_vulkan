#pragma once

#include "evk/ui/event_bus.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace evk {

// 在 UI 上持有的任务句柄。销毁/替换会取消排队任务及尚未交付的回调；
// 已运行的纯计算允许结束，不强行终止线程。完成/失败回调只在 UI 上执行。
class WorkerTask {
public:
    WorkerTask() = default;
    ~WorkerTask() { cancel(); }
    WorkerTask(WorkerTask&& other) noexcept : canceled_(std::move(other.canceled_)) {}
    WorkerTask& operator=(WorkerTask&& other) noexcept {
        if (this != &other) { cancel(); canceled_ = std::move(other.canceled_); }
        return *this;
    }
    WorkerTask(const WorkerTask&) = delete;
    WorkerTask& operator=(const WorkerTask&) = delete;
    void cancel() { if (canceled_) canceled_->store(true); canceled_.reset(); }
    explicit operator bool() const { return canceled_ != nullptr; }
private:
    explicit WorkerTask(std::shared_ptr<std::atomic_bool> canceled)
        : canceled_(std::move(canceled)) {}
    std::shared_ptr<std::atomic_bool> canceled_;
    friend class WorkerPool;
};

class WorkerPool {
public:
    // 默认 1~4 个线程，最多 64 个等待任务；满时返回空句柄，调用方可下次重试。
    explicit WorkerPool(size_t threads = 0, size_t maxPending = 64);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    static WorkerPool& instance();

    // work 必须是普通函数/无捕获 lambda（lambda 用一元 + 转函数指针）。
    // Input/Result 必须拥有数据，不能内含 UI 指针、引用或共享可变状态。
    // C++ 只能拒绝顶层指针/引用，嵌套成员的所有权由调用者保证。
    // 保留返回句柄直到完成；State::dispose 中 cancel，禁止 worker 操作 UI/Vulkan。
    template <typename Input, typename Result, typename Complete>
    WorkerTask compute(Input input, Result (*work)(Input), Complete complete,
                       std::function<void(std::exception_ptr)> onError = {}) {
        static_assert(!std::is_pointer_v<Input> && !std::is_reference_v<Input>);
        static_assert(!std::is_pointer_v<Result> && !std::is_reference_v<Result> &&
                      !std::is_void_v<Result>);
        if (!work) return {};
        auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto alive = alive_;
        // shared_ptr 仅用来装载 move-only 值供 std::function 搬运，无并发访问。
        auto message = std::make_shared<Input>(std::move(input));
        auto completion = std::make_shared<Complete>(std::move(complete));
        auto job = [message, completion, work, onError = std::move(onError),
                    canceled, alive]() mutable {
            if (canceled->load() || !alive->load()) return;
            try {
                auto result = std::make_shared<Result>(work(std::move(*message)));
                if (canceled->load() || !alive->load()) return;
                ui::postUi([result, completion, canceled, alive]() mutable {
                    if (!canceled->load() && alive->load()) (*completion)(std::move(*result));
                });
            } catch (...) {
                auto error = std::current_exception();
                if (onError) ui::postUi([error, onError = std::move(onError), canceled, alive] {
                    if (!canceled->load() && alive->load()) onError(error);
                });
            }
        };
        if (!enqueue(std::move(job))) return {};
        return WorkerTask(std::move(canceled));
    }

private:
    bool enqueue(std::function<void()> job);
    struct Impl;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    std::unique_ptr<Impl> impl_;
};

} // namespace evk
