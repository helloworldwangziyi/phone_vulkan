/**
 * @file widget.cpp
 * @brief 声明式 UI 层实现（仿 Flutter 的 Widget/Element 极简模型）。
 *
 * reconcile 规则（对齐 Flutter 无 key 子节点语义）：
 * - 同位置同类型（typeid）→ updateView 就地更新（滚动 offset 等内部状态保留），
 *   子节点按下标配对递归；
 * - 同位置不同类型 → teardown 销毁重建；
 * - 多出的新子节点创建、多出的旧子节点销毁。
 *
 * 回调槽：控件 C ABI 是函数指针+user_data；每种回调一个静态 trampoline，
 * user_data 指向 Element 持有的堆槽。createView 注册一次，updateView 只覆写
 * 槽内容（user_data 指针不变，控件无需重注册）；teardown 先清空槽再销毁视图。
 *
 * Component 生命周期：pushPage 时 build+挂载+注册 nav 钩子转发虚函数；
 * pop 时 Navigation 先回调 componentPopHook（delete Component，只清槽与事件，
 * 视图树由 Navigation 随后销毁）；整树销毁（如 App 退出/重建）前 App 调
 * teardownAllComponents。
 */

#include "evk/ui/widget.h"

#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/controls/scroll_view.h"
#include "evk/ui/view.h"

namespace evk::ui {
namespace {

// ---- 回调 trampoline：user_data 均为 Element 持有的堆槽 ----

/// click 回调 trampoline。
void clickSlotTrampoline(esx_view view, const esx_view_click_event* /*event*/,
                         void* userData) {
    const auto& fn = static_cast<Slot*>(userData)->fn;
    if (fn) {
        fn(view);
    }
}

/// (view, user_data) 形回调 trampoline：draw callback 与 button click 同形。
void slotViewTrampoline(esx_view view, void* userData) {
    const auto& fn = static_cast<Slot*>(userData)->fn;
    if (fn) {
        fn(view);
    }
}

/// 滚动回调 trampoline。
void scrollSlotTrampoline(esx_view /*view*/, float offsetX, float offsetY,
                          void* userData) {
    const auto& fn = static_cast<ScrollSlot*>(userData)->fn;
    if (fn) {
        fn(offsetX, offsetY);
    }
}

/// 应用背景色；color=0 时清除背景。
void applyBoxColor(esx_view view, uint32_t color) {
    if (color != 0) {
        esx_view_set_background(view, color);
    } else {
        esx_view_clear_background(view);
    }
}

/**
 * @brief ScrollW 的宿主视图。
 *
 * bounds 变化时同步 viewport/content/内容子视图
 * （Column 铺满内容区，宽随 viewport，高 = contentHeight）。
 */
class ScrollFollow : public View {
public:
    esx_view scroll = 0;
    float contentHeight = 0.0f;

    void handleBoundsChanged() override {
        if (scroll == 0) {
            return;
        }
        esx_view_set_bounds(scroll, 0.0f, 0.0f, rect.w, rect.h);
        esx_scroll_view_set_content_size(scroll, rect.w, contentHeight);
        View* content = esxViewFromHandle(esx_scroll_view_get_content(scroll));
        if (content && !content->children.empty()) {
            esx_view_set_bounds(content->children[0]->handle, 0.0f, 0.0f, rect.w,
                                contentHeight);
        }
    }
};

// ---- Component 页面映射与导航钩子 ----

std::unordered_map<esx_view, Component*> g_components;

/// nav 生命周期钩子：转发到 Component 虚函数；返回 1 拦截导航。
int32_t componentNavHook(esx_view /*nav*/, esx_view /*page*/,
                         esx_view_nav_event event, int32_t forward,
                         void* userData) {
    auto* component = static_cast<Component*>(userData);
    const bool f = forward != 0;
    switch (event) {
        case ESX_VIEW_NAV_WILL_ENTER:
            return component->onWillEnter(f) ? 0 : 1;
        case ESX_VIEW_NAV_DID_ENTER:
            component->onDidEnter(f);
            return 0;
        case ESX_VIEW_NAV_WILL_LEAVE:
            return component->onWillLeave(f) ? 0 : 1;
        case ESX_VIEW_NAV_DID_LEAVE:
            component->onDidLeave(f);
            return 0;
    }
    return 0;
}

/// pop 钩子：delete Component（视图树由 Navigation 在 on_pop 返回后销毁）。
void componentPopHook(esx_view /*nav*/, esx_view page, void* /*userData*/) {
    auto it = g_components.find(page);
    if (it == g_components.end()) {
        return;
    }
    Component* component = it->second;
    g_components.erase(it);
    delete component; // 视图树由 Navigation 在 on_pop 返回后销毁
}

/// 事件总线 trampoline：user_data 为 Listener 持有的 shared_ptr 内的 function。
int32_t componentEventTrampoline(int32_t /*eventId*/, const void* data,
                                 void* userData) {
    const auto& fn =
        *static_cast<const std::function<void(const void*)>*>(userData);
    if (fn) {
        fn(data);
    }
    return 0;
}

} // namespace

// ---- Widget 基类 ----

std::vector<std::unique_ptr<Widget>>& Widget::childSpecs() {
    static std::vector<std::unique_ptr<Widget>> empty;
    return empty;
}

// ---- Box ----

esx_view Box::createView(Element& owner) const {
    const esx_view view = esx_create_view(0);
    if (view == 0) {
        return 0;
    }
    applyBoxColor(view, color);
    // 槽位固定：slots[0]=click、slots[1]=draw，fn 可空（trampoline 判空）。
    // click 回调仅在 onTap 非空时注册——无 onTap 的 Box 不应成为触控目标。
    auto clickSlot = std::make_unique<Slot>();
    auto drawSlot = std::make_unique<Slot>();
    if (onTap) {
        clickSlot->fn = [f = onTap](esx_view) { if (f) f(); };
        esx_view_set_click_callback(view, &clickSlotTrampoline, clickSlot.get());
    }
    drawSlot->fn = onDraw;
    esx_view_set_draw_callback(view, &slotViewTrampoline, drawSlot.get());
    owner.slots.push_back(std::move(clickSlot));
    owner.slots.push_back(std::move(drawSlot));
    return view;
}

void Box::updateView(esx_view view, Element& owner) const {
    applyBoxColor(view, color);
    Slot* clickSlot = owner.slots[0].get();
    Slot* drawSlot = owner.slots[1].get();
    View* v = esxViewFromHandle(view);
    if (onTap) {
        clickSlot->fn = [f = onTap](esx_view) { if (f) f(); };
        if (v && v->clickFunc == nullptr) {
            esx_view_set_click_callback(view, &clickSlotTrampoline, clickSlot);
        }
    } else {
        clickSlot->fn = nullptr;
        if (v && v->clickFunc != nullptr) {
            esx_view_set_click_callback(view, nullptr, nullptr);
        }
    }
    drawSlot->fn = onDraw;
}

// ---- ButtonW ----

esx_view ButtonW::createView(Element& owner) const {
    auto slot = std::make_unique<Slot>();
    slot->fn = [f = onTap](esx_view) { if (f) f(); };
    const esx_view view =
        esx_button_create(0, &style, &slotViewTrampoline, slot.get());
    if (view == 0) {
        return 0;
    }
    owner.slots.push_back(std::move(slot));
    return view;
}

void ButtonW::updateView(esx_view view, Element& owner) const {
    esx_button_set_style(view, &style);
    if (!owner.slots.empty()) {
        owner.slots[0]->fn = [f = onTap](esx_view) { if (f) f(); };
    }
}

// ---- ColumnW / RowW ----

esx_view ColumnW::createView(Element& /*owner*/) const {
    const esx_view view = esx_flex_create(0, 1);
    if (view != 0 && color != 0) {
        esx_view_set_background(view, color);
    }
    return view;
}

void ColumnW::updateView(esx_view view, Element& /*owner*/) const {
    applyBoxColor(view, color);
}

void ColumnW::configureChild(esx_view container, const Widget& child,
                             esx_view childView) const {
    const FlexSpec& l = child.layout;
    const esx_flex_child spec{l.main,
                              l.weight,
                              l.cross,
                              static_cast<esx_flex_align>(l.align),
                              l.marginMainBefore,
                              l.marginMainAfter,
                              l.marginCross};
    esx_flex_set_child(container, childView, &spec);
}

esx_view RowW::createView(Element& /*owner*/) const {
    const esx_view view = esx_flex_create(0, 0);
    if (view != 0 && color != 0) {
        esx_view_set_background(view, color);
    }
    return view;
}

// ---- ScrollW ----

esx_view ScrollW::createView(Element& owner) const {
    auto follow = std::make_unique<ScrollFollow>();
    follow->contentHeight = contentHeight;
    ScrollFollow* raw = follow.get();
    const esx_view handle = esxAdoptViewNode(std::move(follow), 0);
    if (handle == 0) {
        return 0;
    }
    raw->scroll = esx_scroll_view_create(0, contentHeight, handle);
    if (raw->scroll == 0) {
        return handle;
    }
    if (onScroll) {
        auto slot = std::make_unique<ScrollSlot>();
        slot->fn = onScroll;
        esx_scroll_view_set_on_scroll(raw->scroll, &scrollSlotTrampoline, slot.get());
        owner.scrollSlots.push_back(std::move(slot));
    }
    return handle;
}

void ScrollW::updateView(esx_view view, Element& owner) const {
    auto* follow = dynamic_cast<ScrollFollow*>(esxViewFromHandle(view));
    if (follow) {
        follow->contentHeight = contentHeight;
        follow->handleBoundsChanged();
    }
    if (!owner.scrollSlots.empty()) {
        owner.scrollSlots[0]->fn = onScroll;
    }
}

esx_view ScrollW::childParent(esx_view view) const {
    auto* follow = dynamic_cast<ScrollFollow*>(esxViewFromHandle(view));
    return follow ? esx_scroll_view_get_content(follow->scroll) : 0;
}

// ---- reconcile ----

void teardown(std::unique_ptr<Element>& slot) {
    if (!slot) {
        return;
    }
    // 先清子 Element（释放回调槽；子视图随父树销毁），再销毁整棵子树。
    slot->children.clear();
    if (slot->view != 0) {
        esx_destroy_view(slot->view);
    }
    slot.reset();
}

/// 子节点 reconcile：按下标配对，多退少补。
static void reconcileChildren(Element& el, Widget& spec) {
    auto& newKids = spec.childSpecs();
    auto& oldKids = el.children;
    const esx_view mount = spec.childParent(el.view);
    for (size_t i = 0; i < newKids.size(); ++i) {
        if (i >= oldKids.size()) {
            oldKids.emplace_back(nullptr);
        }
        reconcile(oldKids[i], std::move(newKids[i]), mount);
        if (oldKids[i]) {
            spec.configureChild(el.view, *oldKids[i]->widget, oldKids[i]->view);
        }
    }
    while (oldKids.size() > newKids.size()) {
        teardown(oldKids.back());
        oldKids.pop_back();
    }
}

void reconcile(std::unique_ptr<Element>& slot, std::unique_ptr<Widget> spec,
               esx_view parentView) {
    if (!spec) {
        teardown(slot);
        return;
    }
    if (slot && slot->widget->sameKind(*spec)) {
        Element& el = *slot;
        spec->updateView(el.view, el);
        reconcileChildren(el, *spec);
        el.widget = std::move(spec);
        return;
    }
    teardown(slot);
    auto el = std::make_unique<Element>();
    el->view = spec->createView(*el);
    if (el->view == 0) {
        EVK_LOGW("reconcile: createView failed");
        return;
    }
    el->widget = std::move(spec);
    if (parentView != 0) {
        esxAdoptChild(parentView, el->view);
    }
    slot = std::move(el);
    reconcileChildren(*slot, *slot->widget);
}

// ---- Component ----

Component::~Component() {
    unregisterListeners();
    // 只释放回调槽，不销毁视图（视图树由 Navigation/teardownAllComponents
    // 之外的流程销毁）。
    root_.reset();
}

void Component::setState(std::function<void()> mutate) {
    if (mutate) {
        mutate();
    }
    if (!root_) {
        return;
    }
    std::unique_ptr<Widget> spec = build();
    if (!root_->widget->sameKind(*spec)) {
        EVK_LOGW("Component::setState: root widget type changed, ignored");
        return;
    }
    reconcile(root_, std::move(spec), 0);
}

void Component::listen(int32_t eventId, esx_event_priority priority,
                       std::function<void(const void* data)> handler) {
    Listener listener;
    listener.eventId = eventId;
    listener.priority = priority;
    listener.handler =
        std::make_shared<std::function<void(const void*)>>(std::move(handler));
    listeners_.push_back(std::move(listener));
    if (view() != 0) {
        registerListeners(); // 已挂载：新监听立即生效
    }
}

void Component::registerListeners() {
    for (Listener& listener : listeners_) {
        if (!listener.registered) {
            esx_event_on(listener.eventId, listener.priority, view(),
                         &componentEventTrampoline, listener.handler.get());
            listener.registered = true;
        }
    }
}

void Component::unregisterListeners() {
    for (Listener& listener : listeners_) {
        if (listener.registered) {
            esx_event_off(listener.eventId, &componentEventTrampoline,
                          listener.handler.get());
            listener.registered = false;
        }
    }
}

void pushPage(esx_view nav, std::unique_ptr<Component> component, bool animated) {
    if (!component) {
        return;
    }
    component->nav_ = nav;
    reconcile(component->root_, component->build(), 0);
    const esx_view page = component->view();
    if (page == 0) {
        EVK_LOGW("pushPage: build produced no view");
        return;
    }
    esx_view_set_nav_callback(page, &componentNavHook, component.get());
    g_components[page] = component.get();
    esx_navigation_set_on_pop(nav, &componentPopHook, nullptr);
    component->registerListeners();
    Component* raw = component.release();

    esx_navigation_push(nav, page, animated ? 1 : 0);
    // push 被页面钩子取消：页面未被接管，清理 Component 与未挂载视图树。
    View* pageView = esxViewFromHandle(page);
    if (pageView && pageView->parent == nullptr && esxRootView() != pageView) {
        g_components.erase(page);
        esx_destroy_view(page);
        delete raw;
    }
}

void teardownAllComponents() {
    while (!g_components.empty()) {
        auto it = g_components.begin();
        Component* component = it->second;
        g_components.erase(it);
        delete component; // 只清槽与事件监听；视图由 App 随后整树销毁
    }
}

} // namespace evk::ui
