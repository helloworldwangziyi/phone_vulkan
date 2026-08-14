#pragma once

// 首页（声明式）：渐变面板 + 详情页/主题按钮 + 模拟行情列表。
// 页面结构全部写在 build() 里：创建了什么、挂在哪，一眼可见。
// 数据流：进入页面（onDidEnter）发起模拟行情请求，数据返回 setState 渲染；
// 离开（onWillLeave）取消请求，迟到数据丢弃。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "evk/ui/widget.h"

class HomePage : public evk::ui::Component {
public:
    HomePage();
    ~HomePage() override;

    std::unique_ptr<evk::ui::Widget> build() override;
    void onDidEnter(bool forward) override;
    bool onWillLeave(bool forward) override;

private:
    bool panelAccent_ = false;
    int detailCount_ = 0;              // push 过的详情页数量（层序号）
    bool quoteLoaded_ = false;
    bool quotePending_ = false;
    std::vector<uint32_t> quotes_;     // 模拟行情数据（颜色行）
    std::shared_ptr<std::atomic_bool> quoteCancel_;
};
