#pragma once

#include "evk/esx_view.h"

// 创建 panel（深蓝背景，Draw 回调里画 RGB 渐变三角形）并挂到 parent 下。
esx_view panelViewCreate(esx_view parent);
