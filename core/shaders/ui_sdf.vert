#version 450

// SDF 顶点着色器：与 ui_vertex 同源，多输出一份屏幕像素坐标（fragPos）
// 给片元做距离场计算。fragPos 是视觉空间坐标（mvp 只做 NDC 旋转补偿，
// 不影响 SDF 在视觉空间的一致性）。
layout(location = 0) in vec2 inPosition; // 屏幕像素坐标
layout(location = 1) in vec4 inColor;    // 顶点色（RGB + A）
layout(location = 2) in vec2 inUv;       // 纹理坐标（纯色几何恒为 0）

layout(push_constant) uniform Push {
    mat4 mvp;          // 像素坐标 → NDC
    vec4 effectRect;   // 效果矩形 x,y,w,h（本阶段不用，占位保持布局一致）
    vec4 effectParams; // radius, blur, mode, pad（同上）
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUv;
layout(location = 2) out vec2 fragPos;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragUv = inUv;
    fragPos = inPosition;
}
