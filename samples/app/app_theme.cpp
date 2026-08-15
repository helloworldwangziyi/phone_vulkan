/// @file app_theme.cpp
#include "app_theme.h"

/// ============================================================================
/// 双主题 token 表。换肤只做两件事：切 g_dark → 页面重跑 build() 重读 token。
/// 任何控件/页面想支持换肤，只需「颜色取 token + 订阅主题切换事件」，
/// 不需要额外的样式管理代码。
/// ============================================================================

namespace {

/// 深色：Slate 灰蓝底 + Indigo/Cyan 主色。
const AppTheme kDarkTheme{
    /*windowBackground*/ 0x0F172AFF,
    /*surface*/ 0x1E293BFF,
    /*surfaceRaised*/ 0x334155FF,
    /*primary*/ 0x6366F1FF,
    /*primaryPressed*/ 0x4F46E5FF,
    /*primaryDisabled*/ 0x475569FF,
    /*secondary*/ 0x334155FF,
    /*secondaryPressed*/ 0x475569FF,
    /*accent*/ 0x22D3EEFF,
    /*navBar*/ 0x0B1220FF,
    /*navBarLine*/ 0x334155FF,
    /*backArrow*/ 0x22D3EEFF,
    /*panelGradient*/ {0x6366F1FF, 0x22D3EEFF, 0xA78BFAFF},
    /*panelAccent*/ 0xF472B6FF,
    /*scrollItems*/ {0x312E81FF, 0x3730A3FF, 0x4338CAFF, 0x4F46E5FF,
                     0x6366F1FF, 0x818CF8FF, 0xA5B4FCFF, 0xC7D2FEFF},
};

/// 浅色：米白灰底 + 同系 Indigo 主色（加深以保证对比度）。
const AppTheme kLightTheme{
    /*windowBackground*/ 0xF1F5F9FF,
    /*surface*/ 0xFFFFFFFF,
    /*surfaceRaised*/ 0xE2E8F0FF,
    /*primary*/ 0x4F46E5FF,
    /*primaryPressed*/ 0x4338CAFF,
    /*primaryDisabled*/ 0xCBD5E1FF,
    /*secondary*/ 0xE2E8F0FF,
    /*secondaryPressed*/ 0xCBD5E1FF,
    /*accent*/ 0x0891B2FF,
    /*navBar*/ 0xFFFFFFFF,
    /*navBarLine*/ 0xCBD5E1FF,
    /*backArrow*/ 0x4F46E5FF,
    /*panelGradient*/ {0x818CF8FF, 0x67E8F9FF, 0xC4B5FDFF},
    /*panelAccent*/ 0xDB2777FF,
    /*scrollItems*/ {0x312E81FF, 0x3730A3FF, 0x4338CAFF, 0x4F46E5FF,
                     0x6366F1FF, 0x818CF8FF, 0xA5B4FCFF, 0xC7D2FEFF},
};

bool g_dark = true;

} ///< namespace

const AppTheme& appTheme() {
    return g_dark ? kDarkTheme : kLightTheme;
}

bool appThemeIsDark() {
    return g_dark;
}

void appThemeToggle() {
    g_dark = !g_dark;
}
