#version 450

// 可分离高斯模糊片元：经典 5 采样线性优化核（9 采样对称核经双线性
// 折叠成 5 次采样），采样步长按目标 sigma 缩放后由 push constant 传入
// （effectParams.xy = uv 步长向量：H 通道 (step,0)，V 通道 (0,step)）。
// 两趟（H+V）即近似二维高斯，效果是 BackdropFilter 式的背景虚化。
layout(location = 0) in vec4 fragColor; // 恒白，未用
layout(location = 1) in vec2 fragUv;
layout(location = 2) in vec2 fragPos;   // 未用，保持与顶点着色器接口一致

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 effectRect;
    vec4 effectParams; // xy = uv 采样步长
    vec4 effectExtra;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 stepUv = pc.effectParams.xy;
    vec4 c = texture(uTexture, fragUv) * 0.2270270270;
    c += (texture(uTexture, fragUv + stepUv * 1.3846153846) +
          texture(uTexture, fragUv - stepUv * 1.3846153846)) * 0.3162162162;
    c += (texture(uTexture, fragUv + stepUv * 3.2307692308) +
          texture(uTexture, fragUv - stepUv * 3.2307692308)) * 0.0702702703;
    outColor = c;
}
