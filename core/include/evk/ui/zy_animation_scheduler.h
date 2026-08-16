#pragma once

#include <cstdint>
#include <functional>

namespace evk::ui {

using AnimationTick = std::function<bool(int64_t frameTimeNanos)>;

void startAnimation(AnimationTick tick);
void tickAnimations(int64_t frameTimeNanos);
void stopAllAnimations();

float easeOutCubic(float t);
float easeInOutCubic(float t);

} // namespace evk::ui
