#pragma once

#include <cstdint>

namespace evk {

// Events reported from the platform shell (Android/iOS/OHOS) up to the app.
// The concrete set of events is decided by the app; extend as needed.
enum class EventId : int32_t {
    AppStart         = 1,
    SurfaceChanged   = 2,
    SurfaceDestroyed = 3,
    Touch            = 1001,
    UiClick          = 1002,
    Draw             = 1003,
};

struct SurfaceChangedData {
    int32_t width;
    int32_t height;
};

struct TouchData {
    int32_t action; // Android MotionEvent action constant, passed through as-is.
    float x;
    float y;
};

// UiClick：视图被点击。view 为被点击视图的句柄（见 esx_view.h），
// x/y 为相对该视图左上角的局部坐标。
struct UiClickData {
    uint32_t view;
    float x;
    float y;
};

// Draw：每帧对每个可见视图回调一次，App 在回调里调 esx_draw_* 在该视图上绘制。
struct DrawData {
    uint32_t view;
};

// App-side event callback. The data pointer (a per-event struct, e.g.
// TouchData for EventId::Touch) is only valid for the duration of the call.
using EventFunc = void (*)(EventId id, const void* data);

// Register the app event callback. Called once at app startup.
void setEventFunc(EventFunc func);

// Single dispatch entry used by every platform shell. Forwards to the
// registered EventFunc; drops the event when none is registered.
void dispatchEvent(EventId id, const void* data);

} // namespace evk
