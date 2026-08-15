/**
 * @file event_bus.cpp
 * @brief 事件总线实现：注册表 + 优先级派发 + 页面可见作用域 + 跨线程 UI 任务队列。
 *
 * 注册表用 vector（注册数量小），派发时快照并按 (priority, order) 稳定排序，
 * 回调中注销安全：off 只置 active=false 并从表中移除，快照里的条目调用前
 * 复查 active 标志。
 */

#include "evk/ui/event_bus.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "evk/ui/view.h"

namespace {

/**
 * @brief 一条事件注册记录。用 shared_ptr 而非值存放：派发时先拷快照，
 * 注册表与快照共享同一条目——回调中注销（off）只改 active/从表移除，
 * 本轮快照仍能安全调用（调用前复查 active）。
 */
struct EventEntry {
    int32_t eventId = 0; ///< 事件号（App 自定义，从 1 起）
    esx_event_priority priority = ESX_PRI_NORMAL; ///< 派发优先级（HIGH 先）
    int64_t order = 0; ///< 注册顺序，同优先级内稳定派发
    esx_view scope = 0; ///< 0=全局；否则仅该视图可见时派发
    esx_event_func func = nullptr; ///< 回调；返回非 0 = 消费并停止派发
    void* userData = nullptr; ///< 注册时传入的用户指针
    bool active = true; ///< off 时置 false（配合快照复查）
};

/// 事件注册表（注册数量小，vector 足够）。
std::vector<std::shared_ptr<EventEntry>> g_entries;
/// 全局注册序号，单调递增（保证同优先级 FIFO）。
int64_t g_nextOrder = 0;

/**
 * @brief 一条待办 UI 任务（esx_post_ui 投递的最小单元）。
 */
struct UiTask {
    esx_task_func func; ///< 任务回调
    void* userData;     ///< 透传给任务的用户指针
};

/**
 * @brief 跨线程任务队列：esx_post_ui 可在任意线程调用（上锁入队），
 * esxDrainUiTasks 在 UI 线程 beginFrame 开头出队执行——
 * 这是"后台数据源 → UI 线程"的唯一合法通道（UI 单线程模型）。
 */
std::mutex g_taskMutex;
std::vector<UiTask> g_tasks;

/**
 * @brief scope 有效性判定：视图自身及父链全部可见，且挂在当前根视图树上。
 *
 * 页面被 Navigation 覆盖（visible=false）时其后代注册者收不到事件
 * = 隐式 pause；整树拆除后（父链断裂）同样不派发。全局 scope=0 不走此判定。
 * @param scope 作用域视图句柄
 * @return true 表示可向该 scope 的注册者派发
 */
bool scopeVisible(esx_view scope) {
    evk::ui::View* view = esxViewFromHandle(scope);
    if (!view) {
        return false;
    }
    evk::ui::View* top = view;
    for (evk::ui::View* v = view; v; v = v->parent) {
        if (!v->visible) {
            return false;
        }
        top = v;
    }
    return top == esxRootView();
}

} // namespace

extern "C" {

void esx_event_on(int32_t event_id, esx_event_priority priority, esx_view scope,
                  esx_event_func func, void* user_data) {
    if (!func) {
        return;
    }
    auto entry = std::make_shared<EventEntry>();
    entry->eventId = event_id;
    entry->priority = priority;
    entry->order = g_nextOrder++;
    entry->scope = scope;
    entry->func = func;
    entry->userData = user_data;
    g_entries.push_back(std::move(entry));
}

void esx_event_off(int32_t event_id, esx_event_func func, const void* user_data) {
    for (auto& entry : g_entries) {
        if (entry->eventId == event_id && entry->func == func &&
            entry->userData == user_data) {
            entry->active = false;
        }
    }
    g_entries.erase(
        std::remove_if(g_entries.begin(), g_entries.end(),
                       [](const std::shared_ptr<EventEntry>& e) { return !e->active; }),
        g_entries.end());
}

void esx_event_emit(int32_t event_id, const void* data) {
    // 快照：派发过程中允许 on/off/emit 重入，不影响本轮名单。
    std::vector<std::shared_ptr<EventEntry>> snapshot;
    for (const auto& entry : g_entries) {
        if (entry->eventId == event_id) {
            snapshot.push_back(entry);
        }
    }
    std::stable_sort(snapshot.begin(), snapshot.end(),
                     [](const std::shared_ptr<EventEntry>& a,
                        const std::shared_ptr<EventEntry>& b) {
                         if (a->priority != b->priority) {
                             return a->priority < b->priority;
                         }
                         return a->order < b->order;
                     });
    for (const auto& entry : snapshot) {
        if (!entry->active) {
            continue;
        }
        if (entry->scope != 0 && !scopeVisible(entry->scope)) {
            continue;
        }
        if (entry->func(event_id, data, entry->userData) != 0) {
            break;
        }
    }
}

void esx_post_ui(esx_task_func func, void* user_data) {
    if (!func) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_taskMutex);
    g_tasks.push_back({func, user_data});
}

} // extern "C"

namespace {

/**
 * @brief std::function 版投递的桥接：userData 是堆上的 std::function，
 * 任务执行后自行 delete——所有权随任务一次性转移（见 postUi 的 new）。
 */
void uiFunctionTrampoline(void* userData) {
    auto* fn = static_cast<std::function<void()>*>(userData);
    (*fn)();
    delete fn;
}

} // namespace

namespace evk::ui {

void postUi(std::function<void()> fn) {
    esx_post_ui(&uiFunctionTrampoline, new std::function<void()>(std::move(fn)));
}

} // namespace evk::ui

/**
 * @brief 执行并清空待办任务（render_loop 的 beginFrame 开头调用，先于 dirty 检查，
 * 保证"数据到达 → setState → 本帧绘制"零额外延迟）。
 *
 * swap 出队而非边执行边出队：执行期间新 post 的任务留在队列等下帧；
 * 只有入队/出队上锁，任务执行本身不加锁（UI 线程独占执行）。
 */
void esxDrainUiTasks() {
    std::vector<UiTask> pending;
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        pending.swap(g_tasks);
    }
    for (const UiTask& task : pending) {
        task.func(task.userData);
    }
}
