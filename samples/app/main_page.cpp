// ============================================================================
// 首页实现：build() 即页面结构（Widget 嵌套 = 视图树父子关系），回调内联。
//
// 对照 estarx App 页面 create 函数的可读性，但消灭了 layout/destroy 样板：
//   - 布局由 Column/Flex 容器自动完成（子节点只声明 layout 参数）；
//   - resize 由容器级联重排（SurfaceChanged 只改根视图 bounds）；
//   - 视图树销毁由 Navigation/框架负责，页面不用清理 records。
//
// 本页状态 → 像素的完整链路（点任意按钮都会走一遍）：
//   触摸 → 命中测试 → Button 控件状态机（pressed/up）→ onTap 回调
//     → setState(mutate)      // 改成员变量
//     → build()               // 重跑，生成新 Widget 描述树
//     → reconcile             // diff：同类型就地更新 / 异类型销毁重建
//     → requestRender()       // 置 dirty，等下一个 VSync 统一提交
//     → beginFrame → 帧构建（背景→draw 回调→子树）→ Vulkan 渲染
// ============================================================================

#include "main_page.h"

#include <chrono>
#include <thread>

#include "app_metrics.h"
#include "app_theme.h"
#include "detail_page.h"
#include "evk/log.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/view.h"

namespace {

// 面板渐变三角（局部坐标，同旧 drawPanel）。
// 顶点按面板实际尺寸现算，不走 appCalc 设计稿缩放——面板的宽来自屏幕实宽
// （margin 约束），与设计稿比例无关；若顶点也用全局缩放比，屏幕宽高比异于
// 设计稿时三角形会偏离面板中心。
// 注意：这是 draw callback，只在「帧构建」期间被调用（见 esx_view.h），
// 里面 esx_draw_* 写的是当前帧的 Canvas 顶点流，不能在这里改视图树。
void drawPanelGradient(esx_view view, bool accent) {
    const AppTheme& theme = appTheme();
    // accent=false 用三段主题渐变；accent=true 把左顶点换成粉色强调色。
    const uint32_t left = accent ? theme.panelAccent : theme.panelGradient[0];
    const evk::ui::View* v = esxViewFromHandle(view);
    const float w = v ? v->rect.w : 0.0f;
    const float h = v ? v->rect.h : 0.0f;
    // 居中：底边半宽 = 面板宽 40%，垂直方向上下各留 12%。
    const float cx = w * 0.5f;
    const float halfBase = w * 0.4f;
    const float top = h * 0.12f;
    const float bottom = h * 0.88f;
    // 三个顶点的颜色不同，GPU 在三角形内部插值出渐变。
    esx_draw_triangle(view, cx, top, cx + halfBase, bottom, cx - halfBase, bottom,
                      left, theme.panelGradient[1], theme.panelGradient[2]);
}

} // namespace

HomePage::HomePage() {
    // 订阅主题切换事件：换肤的完整链路是
    //   themeButton.onTap → appThemeToggle() → esx_event_emit(kEventThemeChanged)
    //     → 这里（listen 注册的 handler）→ setState() → build() 重读 token
    //     → reconcile 就地更新所有颜色。
    // scope 自动绑定页面根视图：本页被详情页完全覆盖时收不到事件
    // （页面隐式 pause，不浪费重建）；Component 销毁时自动注销。
    listen(kEventThemeChanged, ESX_PRI_NORMAL, [this](const void*) { setState(); });
}

HomePage::~HomePage() {
    // 页面销毁兜底：置取消标志，让可能仍在飞的请求线程发现页面已死。
    if (quoteCancel_) {
        quoteCancel_->store(true);
    }
}

std::unique_ptr<evk::ui::Widget> HomePage::build() {
    using namespace evk::ui;
    const AppTheme& theme = appTheme(); // 每次 build 都重读：换肤后颜色即新主题

    // ---- ① 顶部渐变面板 ----
    // Box = 背景 + 点击 + 自定义绘制（Container+GestureDetector+CustomPaint）。
    auto panel = Box(theme.surface);
    panel.onTap = [this] {
        // setState(mutate)：先执行 mutate 改状态，再 rebuild + reconcile。
        setState([this] { panelAccent_ = !panelAccent_; });
    };
    panel.onDraw = [this](esx_view view) { drawPanelGradient(view, panelAccent_); };
    // 布局参数（纵向 Flex 中生效）：固定高度 + 上下间距 + 左右边距。
    panel.layout.main = appCalcHeight(540.0f);
    panel.layout.marginMainBefore = appCalcHeight(60.0f);
    panel.layout.marginCross = appCalcWidth(100.0f);

    // ---- ② 主按钮：push 详情页 ----
    // 每次 push 层序号递增：连续 push 时每层配色不同，跳转有感知。
    const esx_button_style primaryStyle{theme.primary, theme.primaryPressed,
                                        theme.primaryDisabled};
    auto detailButton = ButtonW(primaryStyle);
    detailButton.onTap = [this] {
        pushPage(nav(), std::make_unique<DetailPage>(detailCount_++), true);
    };
    // 固定高度 + 固定宽度 + 交叉轴居中 + 上边距。
    detailButton.layout.main = appCalcHeight(140.0f);
    detailButton.layout.cross = appCalcWidth(400.0f);
    detailButton.layout.align = ESX_FLEX_ALIGN_CENTER;
    detailButton.layout.marginMainBefore = appCalcHeight(60.0f);

    // ---- ③ 次按钮：切换主题并广播 ----
    // 导航栏样式不属于页面内容（页面被覆盖时也得换），所以这里直接
    // esx_navigation_set_style 就地更新；页面自身内容靠事件广播换肤。
    const esx_button_style secondaryStyle{theme.secondary, theme.secondaryPressed,
                                          theme.primaryDisabled};
    auto themeButton = ButtonW(secondaryStyle);
    themeButton.onTap = [this] {
        appThemeToggle();                     // 换 token 值（app_theme.cpp）
        const AppTheme& next = appTheme();
        const esx_navigation_style navStyle{next.navBar, next.navBarLine,
                                            next.surfaceRaised, next.surface,
                                            next.backArrow};
        esx_navigation_set_style(nav(), &navStyle);  // 导航栏立即换色
        esx_event_emit(kEventThemeChanged, nullptr); // 页面经事件总线换肤
    };
    themeButton.layout = detailButton.layout; // 与主按钮同样式排布

    // ---- ④ 模拟行情列表 ----
    // 数据到达前：占位行；到达后：按数据生成行（reconcile 自动增删）。
    // 这是「数据驱动 UI」的典型写法：同一个 build()，状态不同产物不同。
    std::vector<std::unique_ptr<Widget>> rows;
    float contentHeight = 0.0f;
    if (!quoteLoaded_) {
        // 占位：一块凸起色卡片，等数据（600ms 后 setState 触发重建）。
        rows.push_back(makeWidget(Box(theme.surfaceRaised)
                                      .mainSize(appCalcHeight(400.0f))));
        contentHeight = appCalcHeight(400.0f);
    } else {
        // 数据 → 行 widget 列表（Flutter 的 items.map(...).toList()）。
        rows = mapWidgets(quotes_, [](uint32_t color) {
            return Box(color)
                .mainSize(appCalcHeight(122.0f))
                .marginMain(0.0f, appCalcHeight(30.0f));
        });
        contentHeight = appCalcHeight(24.0f + 152.0f * quotes_.size());
    }
    // ScrollW：包一个 ScrollView，子视图（Column 行列表）铺满内容区。
    // 滚动 offset 由 ScrollView 内部状态持有，reconcile 重建后保留。
    auto list = ScrollW(makeWidget(column(std::move(rows))), contentHeight);
    list.onScroll = [](float x, float y) {
        EVK_LOGI("quote list offset=({:.1f}, {:.1f})", x, y);
    };
    // weight=1 = Expanded：吃掉前面固定项之后的所有剩余主轴空间。
    list.layout.weight = 1;
    list.layout.marginMainBefore = appCalcHeight(40.0f);
    list.layout.marginMainAfter = appCalcHeight(24.0f);
    list.layout.marginCross = appCalcWidth(100.0f);

    // ---- ⑤ 组装页面 ----
    // column = 纵向 Flex；嵌套关系就是视图树：panel/detailButton/themeButton
    // /list 从上到下依次排布。page.color 是页面背景（窗口底色）。
    auto page = column(std::move(panel), std::move(detailButton),
                       std::move(themeButton), std::move(list));
    page.color = theme.windowBackground;
    return makeWidget(std::move(page));
}

void HomePage::onDidEnter(bool /*forward*/) {
    // 进入台前（push 转场结束）→ 发起模拟行情请求。
    // 防重复：已加载或请求进行中则跳过（pop 回来时不重复请求）。
    if (quoteLoaded_ || quotePending_) {
        return;
    }
    // 模拟网络请求：后台线程睡 600ms，数据经 esx_post_ui 回 UI 线程。
    // 关键点：绝不能在工作线程直接改视图/setState —— core 是单 UI 线程
    // 模型，一切状态修改必须回到 UI 线程。
    quotePending_ = true;
    quoteCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = quoteCancel_;
    std::thread([this, cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        // postUi：跨线程投递，任务在下一个 VSync（beginFrame 开头）执行。
        evk::ui::postUi([this, cancel] {
            if (cancel->load()) {
                return; // 已离开/销毁：丢弃迟到数据
            }
            const AppTheme& theme = appTheme();
            quotes_.clear();
            for (int i = 0; i < 20; ++i) {
                quotes_.push_back(theme.scrollItems[i % 8]); // 8 色循环生成 20 行
            }
            quoteLoaded_ = true;
            quotePending_ = false;
            EVK_LOGI("quotes arrived, render {} rows", quotes_.size());
            setState(); // 数据返回 → 重建列表（占位行被 reconcile 替换为 20 行）
        });
    }).detach();
}

bool HomePage::onWillLeave(bool /*forward*/) {
    // 离开（被新页覆盖或将 pop）：取消进行中的请求，迟到数据由取消标志拦截。
    // 返回 true = 允许本次导航；返回 false 可拦截（如未登录/未保存）。
    if (quoteCancel_) {
        quoteCancel_->store(true);
    }
    quotePending_ = false;
    return true;
}
