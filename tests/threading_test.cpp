#include "evk/frame_metrics.h"
#include "evk/frame_pipeline.h"
#include "evk/frame_scheduler.h"
#include "evk/worker_pool.h"
#include "evk/ui/texture_store.h"
#include "ui/texture_store_source.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
template <typename Predicate>
void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void enter() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        assert(cv.wait_for(lock, 5s, [this] { return released; }));
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, 5s, [this] { return entered; }));
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};

void framePipeline() {
    const auto ui = std::this_thread::get_id();
    evk::FramePipeline<int> pipeline;
    Gate first;
    std::atomic<int> consumed{0};
    std::thread::id raster, finished;
    assert(pipeline.start([&] { raster = std::this_thread::get_id(); return true; },
        [&](const int& value) {
            assert(std::this_thread::get_id() == raster && raster != ui);
            if (value == 10) first.enter();
            assert(value == 10 + consumed.load());
            ++consumed;
        }, [&] { finished = std::this_thread::get_id(); }));
    assert(pipeline.produce([](int& frame) { frame = 10; }));
    first.wait();
    assert(pipeline.produce([](int& frame) { frame = 11; }));
    bool built = false;
    assert(!pipeline.produce([&](int&) { built = true; }));
    assert(!built); // 满队列不能执行 paint/消费脏纹理，也不能等待正在阻塞的 Raster。
    first.release();
    until([&] { return consumed == 2; });
    until([&] { return pipeline.produce([](int& frame) { frame = 12; }); });
    until([&] { return consumed == 3; });
    pipeline.stop();
    assert(finished == raster && !pipeline.running());
    assert(!pipeline.produce([](int&) {}));
    assert(!pipeline.failure());
}

void frameShutdownAndFailure() {
    evk::FramePipeline<int> pipeline;
    Gate gate;
    std::atomic<int> consumed{0};
    assert(pipeline.start([] { return true; }, [&](const int&) {
        gate.enter();
        ++consumed;
    }, [] {}));
    assert(pipeline.produce([](int& value) { value = 1; }));
    gate.wait();
    assert(pipeline.produce([](int& value) { value = 2; }));
    auto stopped = std::async(std::launch::async, [&] { pipeline.stop(); });
    until([&] { return !pipeline.running(); });
    assert(stopped.wait_for(0s) != std::future_status::ready);
    gate.release();
    assert(stopped.wait_for(5s) == std::future_status::ready);
    stopped.get();
    assert(consumed == 1); // pending 丢弃，active 完成后才允许平台释放窗口。

    bool cleaned = false;
    evk::FramePipeline<int> failed;
    assert(!failed.start([]() -> bool { throw std::runtime_error("init"); },
                         [](const int&) { assert(false); }, [&] { cleaned = true; }));
    assert(cleaned && failed.failure());

    evk::FramePipeline<int> renderFailed;
    assert(renderFailed.start([] { return true; }, [](const int&) {
        throw std::runtime_error("render");
    }, [] {}));
    assert(renderFailed.produce([](int&) {}));
    until([&] { return !renderFailed.running(); });
    renderFailed.stop();
    assert(renderFailed.failure());
}

void textureSnapshots() {
    auto& store = evk::ui::TextureStore::instance();
    store.reset();
    const uint32_t initial[] = {0x112233FF, 0x445566FF, 0x778899FF, 0xAABBCCFF};
    const auto atlas = store.addTexture(2, 2, initial, false);
    const auto image = store.addTexture(2, 2, initial, true);
    auto first = store.takeUpdates(true);
    assert(first.size() == 2 && store.takeUpdates().empty());
    store.mutablePixels(atlas)[3] = 0x12345678;
    store.markDirtyRegion(atlas, 1, 1, 1, 1);
    auto delta = store.takeUpdates();
    assert(delta.size() == 1 && delta[0].pixels.size() == 1);
    assert(delta[0].region.x == 1 && delta[0].region.y == 1);
    store.mutablePixels(atlas)[3] = 0xFFFFFFFF; // 尚未发布，不能改变已经移交的快照。
    store.markDirtyRegion(atlas, 1, 1, 1, 1);

    auto raster = std::async(std::launch::async, [first = std::move(first), delta = std::move(delta), atlas, image] {
        evk::ui::TextureStoreSource source;
        source.apply(first);
        std::vector<uint8_t> rgba(source.mipChainBytes(atlas));
        assert(source.copyMipChain(atlas, rgba.data(), rgba.size()));
        assert(rgba[0] == 0x11 && rgba[12] == 0xAA);
        assert(source.mipLevelCount(image) == 2 && source.mipChainBytes(image) == 20);
        std::vector<uint8_t> mip(20);
        assert(source.copyMipChain(image, mip.data(), mip.size()));
        assert(mip[16] == 94 && mip[17] == 111 && mip[18] == 128 && mip[19] == 255);
        evk::gpu::TextureRegion region;
        assert(source.consumeDirty(atlas, &region) && region.w == 2);
        source.apply(delta);
        assert(source.consumeDirty(atlas, &region));
        assert(region.x == 1 && region.y == 1 && region.w == 1 && region.h == 1);
        assert(source.copyRegion(atlas, 1, 1, 1, 1, rgba.data(), rgba.size()));
        assert(rgba[0] == 0x12 && rgba[1] == 0x34 && rgba[2] == 0x56 && rgba[3] == 0x78);
        // 上传失败后仍能取全图重试，且不受 UI 对同纹理的下一笔写入影响。
        assert(source.copyMipChain(atlas, rgba.data(), rgba.size()));
        assert(rgba[12] == 0x12 && rgba[15] == 0x78);
    });
    assert(raster.wait_for(5s) == std::future_status::ready);
    raster.get();
    store.takeUpdates();
    evk::ui::TextureStoreSource recreated;
    recreated.apply(store.takeUpdates(true)); // surface 重建，即使 UI 已无脏标记也完整补传。
    assert(recreated.textureCount() == 2 && recreated.consumeDirty(atlas, nullptr));
    store.reset();
}

// 测试探针刻意跨线程共享 gate；业务消息不能包含 UI 对象。
Gate* workerGate = nullptr;
std::atomic<int> workRuns{0};
int blockedWork(int value) { ++workRuns; workerGate->enter(); return value * 2; }
int countWork(int value) { ++workRuns; return value * 2; }

void workerMessages() {
    const auto ui = std::this_thread::get_id();
    evk::WorkerPool pool(1, 1);
    Gate gate;
    workerGate = &gate;
    workRuns = 0;
    bool completed = false;
    auto first = pool.compute(21, blockedWork, [&](int result) {
        assert(std::this_thread::get_id() == ui && result == 42);
        completed = true;
    });
    assert(first);
    gate.wait();
    auto canceled = pool.compute(2, countWork, [](int) { assert(false); });
    assert(canceled);
    auto rejected = pool.compute(3, countWork, [](int) { assert(false); });
    assert(!rejected);
    canceled.cancel();
    assert(!completed);
    gate.release();
    until([&] { evk::ui::drainUiTasks(); return completed; });

    // 输入是值拷贝，结果支持 move-only。FIFO sentinel 确认此前任务已完成投递。
    std::vector<int> input{1, 2, 3};
    bool delivered = false;
    evk::WorkerTask valueTask;
    until([&] {
        valueTask = pool.compute(input, +[](std::vector<int> values) {
            assert(values == std::vector<int>({1, 2, 3}));
            return std::make_unique<int>(6);
        }, [&](std::unique_ptr<int> value) { assert(*value == 6); delivered = true; });
        return bool(valueTask);
    });
    input[0] = 99;
    until([&] { evk::ui::drainUiTasks(); return delivered; });
    assert(workRuns == 1);

    bool errored = false;
    auto error = pool.compute(1, +[](int) -> int { throw std::runtime_error("compute"); },
        [](int) { assert(false); }, [&](std::exception_ptr exception) {
            assert(std::this_thread::get_id() == ui);
            try { std::rethrow_exception(exception); }
            catch (const std::runtime_error&) { errored = true; }
        });
    until([&] { evk::ui::drainUiTasks(); return errored; });
}

void cancelAfterPostingAndShutdown() {
    auto pool = std::make_unique<evk::WorkerPool>(1, 8);
    auto canceled = pool->compute(1, countWork, [](int) { assert(false); });
    Gate sentinel;
    workerGate = &sentinel;
    auto last = pool->compute(2, blockedWork, [](int) { assert(false); });
    sentinel.wait(); // 前一个结果已在 UI 队列中，取消仍须拦截。
    canceled.cancel();
    evk::ui::drainUiTasks();
    auto stopped = std::async(std::launch::async, [&] { pool.reset(); });
    sentinel.release();
    assert(stopped.wait_for(5s) == std::future_status::ready);
    stopped.get();
    evk::ui::drainUiTasks(); // pool 销毁前投递的回调也不能再访问 State。
}

void postUiRequestsFrame() {
    int frames = 0, tasks = 0;
    evk::setFrameFunc([&](int64_t) { ++frames; });
    evk::cancelPendingFrame();
    std::thread producer([&] {
        evk::ui::postUi([&] { ++tasks; });
        evk::ui::postUi([&] { ++tasks; });
    });
    producer.join();
    assert(tasks == 0 && evk::beginFrame(1));
    assert(frames == 1 && tasks == 2);
    assert(!evk::beginFrame(2));
    evk::setFrameFunc({});
}
} // namespace

int main() {
    framePipeline();
    frameShutdownAndFailure();
    textureSnapshots();
    workerMessages();
    cancelAfterPostingAndShutdown();
    postUiRequestsFrame();
    std::cout << "threading_test: all checks passed\n";
}
