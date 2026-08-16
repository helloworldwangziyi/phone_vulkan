#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace evk::ui {

class View;

enum class EventPriority {
    High,
    Normal,
    Low,
};

class EventSubscription {
public:
    struct Entry;

    EventSubscription() = default;
    ~EventSubscription();
    EventSubscription(EventSubscription&& other) noexcept;
    EventSubscription& operator=(EventSubscription&& other) noexcept;

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    void cancel();

private:
    explicit EventSubscription(std::weak_ptr<Entry> entry);

    std::weak_ptr<Entry> entry_;

    friend class EventBus;
};

class EventBus {
public:
    static EventBus& instance();

    EventSubscription subscribe(
        int32_t eventId,
        EventPriority priority,
        View* scope,
        std::function<bool(const void* data)> handler);

    void emit(int32_t eventId, const void* data = nullptr);
    void clear();

private:
    EventBus() = default;
};

void postUi(std::function<void()> task);
void drainUiTasks();

} // namespace evk::ui
