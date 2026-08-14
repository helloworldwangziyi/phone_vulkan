// ============================================================================
// 事件总线实现：注册表 + 优先级派发 + 页面可见作用域 + 跨线程 UI 任务队列。
//
// 注册表用 vector（注册数量小），派发时快照并按 (priority, order) 稳定排序，
// 回调中注销安全：off 只置 active=false 并从表中移除，快照里的条目调用前
// 复查 active 标志。
// ============================================================================

#include "evk/ui/event_bus.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "evk/ui/view.h"

namespace {

struct EventEntry {
    int32_t eventId = 0;
    esx_event_priority priority = ESX_PRI_NORMAL;
    int64_t order = 0; // 注册顺序，同优先级内稳定派发
    esx_view scope = 0;
    esx_event_func func = nullptr;
    void* userData = nullptr;
    bool active = true;
};

std::vector<std::shared_ptr<EventEntry>> g_entries;
int64_t g_nextOrder = 0;

struct UiTask {
    esx_task_func func;
    void* userData;
};

std::mutex g_taskMutex;
std::vector<UiTask> g_tasks;

// scope 视图及其父链全部可见，且挂在当前根视图树上。
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
