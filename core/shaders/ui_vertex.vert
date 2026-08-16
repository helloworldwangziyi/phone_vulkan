#version 450

layout(location = 0) in vec2 inPosition; // 屏幕像素坐标
layout(location = 1) in vec4 inColor;    // 顶点色（RGB + A）
layout(location = 2) in vec2 inUv;       // 纹理坐标（纯色几何恒为 0）

layout(push_constant) uniform Push { mat4 mvp; } pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUv;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragUv = inUv;
}
