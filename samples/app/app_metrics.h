#pragma once

constexpr float kDesignWidth = 1080.0f;
constexpr float kDesignHeight = 2339.0f;

extern float g_screenWidth;
extern float g_screenHeight;

void appSetScreenSize(float width, float height);
float appCalcWidth(float designPixels);
float appCalcHeight(float designPixels);
