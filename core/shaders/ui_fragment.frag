#version 450

// 统一走"纹理 × 顶点色"的着色公式（RGBA 逐通道相乘）：
//   - 字体批次绑定字形 atlas（RGBA，rgb=白、a=笔画覆盖率），
//     输出 = 文字颜色 × 覆盖度，天然得到抗锯齿的文字边缘；
//   - 图像批次绑定业务位图，顶点色作为染色（默认白色即原样贴图）；
//   - 纯色/渐变批次绑定 1x1 白纹理（RGBA 全 1），公式退化为直出顶点色。
// 一套管线同时服务三种绘制，批次切换只换绑定的 descriptor set。
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUv;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 sampled = texture(uTexture, fragUv);
    outColor = vec4(fragColor.rgb * sampled.rgb, fragColor.a * sampled.a);
}
