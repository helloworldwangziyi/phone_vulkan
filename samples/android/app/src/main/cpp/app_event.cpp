#include "app_event.h"

#include <vector>

namespace {

struct Listener {
    evk::EventId id;
    esx_view view; // 0 = 全局监听
    AppEventFunc func;
};

// 函数内静态，规避跨 TU 的静态初始化顺序问题
//（global.cpp 在 .so 加载的静态期就会注册监听）。
std::vector<Listener>& listeners() {
    static std::vector<Listener> s_listeners;
    return s_listeners;
}

// 从 data 中提取事件命中的视图；不带视图的事件返回 0。
esx_view eventView(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::Draw:
            return static_cast<const evk::DrawData*>(data)->view;
        case evk::EventId::UiClick:
            return static_cast<const evk::UiClickData*>(data)->view;
        default:
            return 0;
    }
}

} // namespace

void appRegisterEvent(evk::EventId id, esx_view view, AppEventFunc func) {
    for (const auto& l : listeners()) {
        if (l.id == id && l.view == view && l.func == func) {
            return;
        }
    }
    listeners().push_back(Listener{id, view, func});
}

void appEventEntry(evk::EventId id, const void* data) {
    const esx_view view = eventView(id, data);
    // 快照迭代：允许回调里注册新监听（如 AppStart 中建视图时注册视图回调），
    // 新监听不影响本次分发。
    const auto snapshot = listeners();
    for (const auto& l : snapshot) {
        if (l.id != id) {
            continue;
        }
        if (l.view != 0 && l.view != view) {
            continue; // 视图回调只收落在自己身上的事件
        }
        l.func(id, view, data);
    }
}
