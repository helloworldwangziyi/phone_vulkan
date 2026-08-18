#pragma once

/**
 * @file widgets.h
 * @brief 声明式 UI 对外桶头：框架核心 + 全部控件，一个头文件够用。
 *
 * 对照 Flutter 的 package:flutter/widgets.dart。写页面/组件时包含本头
 * 即可；只派生 StatefulWidget/State 的头文件包含 widget_tree.h 就够。
 */

#include "evk/ui/widget_tree.h"

#include "evk/ui/controls/basic.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/controls/container.h"
#include "evk/ui/controls/flex.h"
#include "evk/ui/controls/image.h"
#include "evk/ui/controls/list_view.h"
#include "evk/ui/controls/scroll_view.h"
#include "evk/ui/controls/text.h"
