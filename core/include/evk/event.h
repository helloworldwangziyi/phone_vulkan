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

// App-side event callback. The data pointer (a per-event struct, e.g.
// TouchData for EventId::Touch) is only valid for the duration of the call.
using EventFunc = void (*)(EventId id, const void* data);

// Register the app event callback. Called once at app startup.
void setEventFunc(EventFunc func);

// Single dispatch entry used by every platform shell. Forwards to the
// registered EventFunc; drops the event when none is registered.
void dispatchEvent(EventId id, const void* data);

} // namespace evk
