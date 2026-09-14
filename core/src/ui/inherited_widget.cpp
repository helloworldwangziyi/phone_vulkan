/**
 * @file inherited_widget.cpp
 * @brief InheritedWidget / InheritedElement 的实现（设计说明见头文件）。
 *
 * @details 变更通知的完整链路：
 * @code
 *   祖先 setState → build 出带新数据的 InheritedWidget
 *     → updateChild 对比：同类型 → Element::update
 *       → willUpdateWidget：新旧都在手，算 updateShouldNotify 存起来
 *       → updateElement：要通知则纪元 +1 → rebuild 转交 child（子树
 *         全递归对比，被触及的读者重新 dependOn 并刷新到当前纪元）
 *         → notifyStaleDependents 补漏：只重建仍停在旧纪元的依赖方
 * @endcode
 *
 * 通知是同步的（与 setState 一致）。补漏遍历走快照：依赖方的 rebuild
 * 可能增删子树、进而让别的节点 unmount 摘除登记，直接迭代 dependents_
 * 会失效。
 */

#include "evk/ui/inherited_widget.h"

#include <utility>

#include "evk/frame_scheduler.h"

namespace evk::ui {

InheritedWidget::InheritedWidget(std::unique_ptr<Widget> child)
    : ProxyWidget(std::move(child)) {}

std::unique_ptr<Element> InheritedWidget::createElement() const {
    return std::make_unique<InheritedElement>();
}

View* InheritedElement::renderObject() const {
    return child_ ? child_->renderObject() : nullptr;
}

void InheritedElement::rebuild() {
    auto& config = dynamic_cast<ProxyWidget&>(*widget_);
    updateChild(child_, std::move(config.child()), renderParent_);
}

void InheritedElement::unmount() {
    if (child_) {
        child_->unmount();
        child_.reset();
    }
    Element::unmount();
}

bool InheritedElement::dispatchRouteEvent(RouteEvent event, bool forward) {
    return !child_ || child_->dispatchRouteEvent(event, forward);
}

void InheritedElement::firstMount() { rebuild(); }

void InheritedElement::willUpdateWidget(const Widget& newWidget) {
    notifyAfterUpdate_ =
        static_cast<const InheritedWidget&>(newWidget).updateShouldNotify(
            static_cast<const InheritedWidget&>(*widget_));
}

void InheritedElement::updateElement() {
    const bool notify = notifyAfterUpdate_;
    notifyAfterUpdate_ = false;
    if (notify) {
        ++notifyEpoch_;
    }
    rebuild();
    if (notify) {
        notifyStaleDependents();
    }
}

void InheritedElement::addDependent(Element* dependent) {
    dependents_[dependent] = notifyEpoch_;
}

void InheritedElement::removeDependent(Element* dependent) {
    dependents_.erase(dependent);
}

void InheritedElement::notifyStaleDependents() {
    std::vector<Element*> stale;
    stale.reserve(dependents_.size());
    for (const auto& entry : dependents_) {
        if (entry.second < notifyEpoch_) {
            stale.push_back(entry.first);
        }
    }
    for (Element* dependent : stale) {
        if (dependent->mounted()) {
            dependent->rebuild();
        }
    }
    requestRender();
}

} // namespace evk::ui
