#include "evk/ui/zy_event_bus.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "evk/ui/zy_render_view.h"

namespace evk::ui {

struct EventSubscription::Entry {
    int32_t eventId = 0;
    EventPriority priority = EventPriority::Normal;
    int64_t order = 0;
    ViewRef scope;
    bool scoped = false;
    std::function<bool(const void*)> handler;
    bool active = true;
};

namespace {

std::vector<std::shared_ptr<EventSubscription::Entry>> g_entries;
int64_t g_nextOrder = 0;
std::mutex g_taskMutex;
std::vector<std::function<void()>> g_tasks;

bool scopeVisible(const ViewRef& reference) {
    View* view = reference.get();
    if (!view) {
        return false;
    }
    View* top = view;
    for (View* current = view; current; current = current->parent) {
        if (!current->visible) {
            return false;
        }
        top = current;
    }
    return top == rootView();
}

} // namespace

EventSubscription::EventSubscription(std::weak_ptr<Entry> entry)
    : entry_(std::move(entry)) {}

EventSubscription::~EventSubscription() {
    cancel();
}

EventSubscription::EventSubscription(EventSubscription&& other) noexcept
    : entry_(std::move(other.entry_)) {}

EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept {
    if (this != &other) {
        cancel();
        entry_ = std::move(other.entry_);
    }
    return *this;
}

void EventSubscription::cancel() {
    if (auto entry = entry_.lock()) {
        entry->active = false;
    }
    entry_.reset();
}

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

EventSubscription EventBus::subscribe(
    int32_t eventId,
    EventPriority priority,
    View* scope,
    std::function<bool(const void*)> handler) {
    auto entry = std::make_shared<EventSubscription::Entry>();
    entry->eventId = eventId;
    entry->priority = priority;
    entry->order = g_nextOrder++;
    entry->scope = ViewRef(scope);
    entry->scoped = scope != nullptr;
    entry->handler = std::move(handler);
    g_entries.push_back(entry);
    return EventSubscription(entry);
}

void EventBus::emit(int32_t eventId, const void* data) {
    g_entries.erase(
        std::remove_if(g_entries.begin(), g_entries.end(),
                       [](const auto& entry) { return !entry->active; }),
        g_entries.end());

    std::vector<std::shared_ptr<EventSubscription::Entry>> snapshot;
    for (const auto& entry : g_entries) {
        if (entry->eventId == eventId) {
            snapshot.push_back(entry);
        }
    }
    std::stable_sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) {
        if (a->priority != b->priority) {
            return a->priority < b->priority;
        }
        return a->order < b->order;
    });
    for (const auto& entry : snapshot) {
        if (!entry->active || (entry->scoped && !scopeVisible(entry->scope))) {
            continue;
        }
        if (entry->handler && entry->handler(data)) {
            break;
        }
    }
}

void EventBus::clear() {
    for (auto& entry : g_entries) {
        entry->active = false;
    }
    g_entries.clear();
}

void postUi(std::function<void()> task) {
    if (!task) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_taskMutex);
    g_tasks.push_back(std::move(task));
}

void drainUiTasks() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        pending.swap(g_tasks);
    }
    for (auto& task : pending) {
        task();
    }
}

} // namespace evk::ui
