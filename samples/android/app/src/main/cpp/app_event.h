#pragma once

// App 侧事件分发器。core 只有一个事件入口（evk::setEventFunc），
// 这里按注册表把事件分发给两类监听（参考 estarxapp 的 esx_common_register_event）：
//   1. 全局监听（view=0）：任何事件都会收到，主要用于给全局变量赋值，
//      统一放在 global.cpp；
//   2. 视图回调（view!=0）：仅事件落在该视图上时触发，
//      例如 Draw 事件触发视图重绘自己。

#include "evk/esx_view.h"
#include "evk/event.h"

// view 为事件命中的视图句柄；不带视图的事件（AppStart/SurfaceChanged/Touch）恒为 0。
using AppEventFunc = void (*)(evk::EventId id, esx_view view, const void* data);

// 注册事件监听。同一 (id, view, func) 重复注册会被忽略。
void appRegisterEvent(evk::EventId id, esx_view view, AppEventFunc func);

// core 事件总入口，在 app_main 注册给 evk::setEventFunc。
void appEventEntry(evk::EventId id, const void* data);
