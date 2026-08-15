/// @file app_main.cpp
/// ============================================================================
/// 跨平台 App 唯一入口（Android/iOS/HarmonyOS 共用这一份源码）。
///
/// 平台薄壳只负责：把 surface 交给渲染器、把触摸/VSync 翻译成 core 事件；
/// 页面、布局和业务回调全部链接这套公共源码。本文件是「事件 → 视图」的
/// 第一站：所有生命周期事件经 evk::dispatchEvent 汇聚到 appEvent()。
///
/// 启动顺序（对照 platform/android/cpp/bridge.cpp）：
///
///   Android 启动
///     → VulkanSurfaceView.surfaceCreated
///     → NativeBridge.nativeInit(Surface)
///     → 平台壳建 Renderer（Vulkan 设备/交换链/管线）
///     → evk::setEngineReady(true)              /// core 进入就绪状态
///     → 报 SurfaceChanged（真实像素尺寸）        /// 此时可安全建视图
///     → 首次报 EngineReady                      /// App 建视图树的信号
///     → appCreateUi()：建 Navigation 根视图 + push 首页
///     → requestRender() → VSync 到达 → 首帧渲染
///
/// 之后的事件只有两类：
///   - SurfaceChanged：屏幕尺寸变化（旋转/折叠屏展开），只调根视图 bounds，
///     页面内容重排由 Flex 容器自动级联完成（App 不写 layout 函数）；
///   - SurfaceDestroyed：surface 销毁（退后台/锁屏），整棵树拆除，
///     下次 surfaceCreated 会重新走一遍上面的 EngineReady 流程。
///
/// 线程模型：所有回调都在 Android UI 线程执行（单 UI 线程模型），
/// App 回调里改状态只 requestRender，不会在回调栈里递归渲染。
/// ============================================================================

#include "app_metrics.h"
#include "app_theme.h"
#include "detail_page.h"
#include "main_page.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/widget.h"

namespace {

/// 唯一的根视图：Navigation（导航容器）。它自己画导航栏，页面挂在其内容区。
esx_view g_nav = 0;

/// 建整棵视图树。引擎就绪（EngineReady）后调用，只建一次。
void appCreateUi() {
    if (g_nav != 0) {
        return;
    }
    /// Navigation 的视觉样式来自主题 token（换肤时经 esx_navigation_set_style 更新）。
    const AppTheme& theme = appTheme();
    const esx_navigation_style navStyle{
        theme.navBar, theme.navBarLine, theme.surfaceRaised,
        theme.surface, theme.backArrow,
    };
    /// 根视图铺满屏幕（此时已收到 SurfaceChanged，g_screenWidth/Height 是真实像素）。
    /// parent=0 创建 = 暂不挂载，下面 set_root_view 才把它定为根。
    /// appCalcHeight(150) 是导航栏高度（设计稿 150px 换算成真实像素）。
    g_nav = esx_navigation_create(0, 0, g_screenWidth, g_screenHeight, 0,
                                  appCalcHeight(150), &navStyle);
    esx_set_root_view(g_nav);
    /// 把首页作为第一页 push 进导航栈（animated=false：首屏无转场）。
    /// pushPage 内部：HomePage::build() → reconcile 挂载视图 → 注册生命周期钩子 → push。
    evk::ui::pushPage(g_nav, std::make_unique<HomePage>(), false);
}

/// 拆整棵视图树。surface 销毁时调用，顺序不能反：
/// 先清页面层（回调槽/事件监听），再销毁视图树（句柄/View 对象）。
void appDestroyUi() {
    if (g_nav == 0) {
        return;
    }
    evk::ui::teardownAllComponents(); ///< ① 先清页面回调槽/事件监听
    esx_destroy_view(g_nav); ///< ② 再销毁整棵视图树（含导航栈里的页面）
    g_nav = 0;
}

/// 生命周期事件入口：平台壳 dispatchEvent → 这里。View 输入不经过此通道
/// （触摸直接走 ui::dispatchPointerEvent），这里只有 surface 级事件。
void appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::SurfaceChanged: {
            /// 数据是 surface 的真实像素尺寸（旋转/折叠屏展开会再次触发）。
            const auto* size = static_cast<const evk::SurfaceChangedData*>(data);
            /// 更新「设计像素 → 真实像素」的换算基准（见 app_metrics）。
            appSetScreenSize(static_cast<float>(size->width),
                             static_cast<float>(size->height));
            /// 根视图跟随屏幕尺寸；页面内容重排由 Flex 容器级联完成。
            if (g_nav != 0) {
                esx_view_set_bounds(g_nav, 0, 0, g_screenWidth, g_screenHeight);
            }
            break;
        }
        case evk::EventId::EngineReady:
            /// 渲染器初始化完成，可安全创建视图。首次 surfaceCreated 时收到。
            appCreateUi();
            break;
        case evk::EventId::SurfaceDestroyed:
            /// surface 没了，渲染资源即将被平台壳释放，视图树整体拆除。
            appDestroyUi();
            break;
    }
}

/// 用全局对象的构造函数在 main() 之前注册事件入口：
/// 平台壳（bridge.cpp）上报第一个事件时，appEvent 一定已经就位，
/// App 侧无需任何显式初始化调用。
struct AppBootstrap {
    AppBootstrap() {
        evk::setEventFunc(appEvent);
    }
};

AppBootstrap g_appBootstrap;

} ///< namespace
