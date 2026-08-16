#version 450

// 统一走"纹理 × 顶点色"的着色公式：
//   - 字体批次绑定字形 atlas（R8 覆盖率图），r 通道是笔画覆盖度，
//     输出 = 文字颜色 × 覆盖度，天然得到抗锯齿的文字边缘；
//   - 纯色/渐变批次绑定 1x1 白纹理（r = 1），公式退化为直接输出顶点色。
// 一套管线同时服务两种绘制，批次切换只换绑定的 descriptor set。
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUv;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 outColor;

void main() {
    float coverage = texture(uTexture, fragUv).r;
    outColor = vec4(fragColor.rgb, fragColor.a * coverage);
}
