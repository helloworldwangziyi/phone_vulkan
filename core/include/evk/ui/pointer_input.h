#pragma once

#include <cstdint>

namespace evk::ui {

class View;

enum class PointerAction {
    Down,
    Move,
    Up,
    Cancel,
};

struct PointerEvent {
    PointerAction action = PointerAction::Down;
    int32_t pointerId = 0;
    float x = 0.0f;
    float y = 0.0f;
    int64_t timeNanos = 0;
};

void dispatchPointerEvent(const PointerEvent& event);
void discardPointerForView(View* view);
void cancelPointerForView(View* view);
void cancelAllPointerEvents();
/// 该视图当前是否正被某根手指的 pan 拖动着（多指下同视图 pan 排他）。
bool isPanGestureActive(View* view);

} // namespace evk::ui
