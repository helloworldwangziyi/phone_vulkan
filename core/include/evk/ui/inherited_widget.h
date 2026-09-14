#pragma once

/**
 * @file inherited_widget.h
 * @brief 树内依赖下发：祖先持有数据，子孙 build 时 dependOn 登记依赖，
 *        数据变更时精确通知依赖它的子树重建（Flutter 的 InheritedWidget）。
 *
 * @details 三棵树里它挂在 Element 层（不产生 View，沿用 ProxyWidget/
 * Element 扩展点），与 Flutter 的对应关系：
 *
 *   - **InheritedWidget**：ProxyWidget 子类。持有一份随 rebuild 下发的
 *     不可变数据（主题、Locale……）。同类型新 Widget 到达时框架问
 *     updateShouldNotify(old)：数据真变了才走通知；
 *   - **InheritedElement**：ProxyElement 的兄弟实现（都是不产生 View、
 *     转交 child 的壳），额外维护一张「谁依赖我、上次登记于哪轮通知」
 *     的登记表；
 *   - **依赖登记**：子孙在 build() 里调
 *     `context.dependOnInheritedWidgetOfExactType<T>()`，框架沿 parent_
 *     链向上找到最近的 T 类型 InheritedElement，读取数据的同时把子孙
 *     登记进依赖表；只读不登记用 findInheritedWidgetOfExactType<T>()；
 *   - **变更通知**：updateShouldNotify 为真时通知纪元（epoch）前进一格，
 *     随后重建在递归中被触及的读者会重新 dependOn、刷新到当前纪元；
 *     通知只补「登记仍停在旧纪元」的依赖方。
 *
 * 为什么通知要按纪元去重：本框架的重建是同步全递归的（updateChild 遍历
 * 所有子位置，StatefulElement 更新即重跑 State::build），InheritedWidget
 * 换数据必然伴随祖先 rebuild，其下方读者天然已被递归重建并读到新值。
 * 若通知再直接 rebuild 一遍，读者会重复构建。纪元机制让通知成为纯
 * 安全网：今天递归是全量的（补漏集恒为空），未来引入脏标记/子树剪枝
 * 后，被跳过的读者仍能由通知兜底重建——Flutter 契约不变，读者恰好
 * 重建一次。
 *
 * 与 Flutter 的两点差异（本框架全局约定的延伸）：
 *   1. 通知是同步完成的（Flutter 延迟到帧末 flush）——build() 必须
 *      便宜；通知回调里允许再次 setState/重建（遍历走快照，增删安全）；
 *   2. 依赖登记不随 rebuild 重评（Flutter 每帧清掉重建）——登记过的
 *      节点持续收通知直到 unmount，多收只是多一次幂等重建。
 *
 * 典型用法（主题）：
 * @code
 *   class AppTheme : public evk::ui::InheritedWidget {
 *   public:
 *       AppTheme(ThemeData data, std::unique_ptr<Widget> child)
 *           : InheritedWidget(std::move(child)), data_(std::move(data)) {}
 *       const ThemeData& data() const { return data_; }
 *       bool updateShouldNotify(const InheritedWidget& old) const override {
 *           return data_ != static_cast<const AppTheme&>(old).data();
 *       }
 *       static const ThemeData& of(BuildContext& context) {
 *           return context.dependOnInheritedWidgetOfExactType<AppTheme>()->data();
 *       }
 *   private:
 *       ThemeData data_;
 *   };
 * @endcode
 */

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 持有下发数据的转接 Widget（ProxyWidget 子类）。
 *
 * 子类把数据存为自身字段，经 of(context) 之类的静态入口暴露；
 * updateShouldNotify 对比新旧数据，决定变更是否通知依赖方。
 */
class InheritedWidget : public ProxyWidget {
public:
    explicit InheritedWidget(std::unique_ptr<Widget> child);

    std::unique_ptr<Element> createElement() const override;

    /**
     * @brief 同类型新 Widget（this）与旧 Widget 的数据是否有实质差异。
     * @param oldWidget 上一版（同类型，框架保证）
     * @return true = 通知依赖方（纪元前进）；false = 静默换副本
     */
    virtual bool updateShouldNotify(const InheritedWidget& oldWidget) const = 0;
};

/**
 * @brief InheritedWidget 对应的 Element：转交 child 的壳 + 依赖登记表。
 *
 * 公开类（不在匿名命名空间）：BuildContext 的 dependOn 查找需要
 * dynamic_cast 识别它。生命周期：dependents_ 里的节点都是本节点子树内
 * 的子孙，unmount 先拆孩子——子孙摘除登记后 dependents_ 自然清空。
 */
class InheritedElement : public Element {
public:
    View* renderObject() const override;

    void rebuild() override;

    void unmount() override;

    bool dispatchRouteEvent(RouteEvent event, bool forward) override;

    /// 依赖登记：记录该节点登记时所处的通知纪元（unmount 时由对方反向摘除）。
    void addDependent(Element* dependent);
    void removeDependent(Element* dependent);

protected:
    void firstMount() override;
    void updateElement() override;
    void willUpdateWidget(const Widget& newWidget) override;

private:
    /// 重建「登记停在旧纪元」的依赖方（被本次 rebuild 递归触及的读者
    /// 已重新登记到当前纪元，不在此列——见头文件注释的纪元说明）。
    void notifyStaleDependents();

    std::unique_ptr<Element> child_;
    std::unordered_map<Element*, uint64_t> dependents_;
    uint64_t notifyEpoch_ = 0;
    bool notifyAfterUpdate_ = false;
};

} // namespace evk::ui
