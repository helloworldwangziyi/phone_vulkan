#version 450

// 无顶点输入的全屏/区域 quad 顶点着色器（模糊/合成/末段 blit 共用）：
// gl_VertexIndex 生成单位四边形（6 顶点），effectRect 给出目标矩形
// （视觉像素坐标），mvp 负责像素 → NDC（末段 blit 时含旋转补偿）。
// effectExtra.xy = 1/纹理尺寸，把像素位置折算成采样 uv——离屏图按
// 视觉尺寸分配，uv 即像素位置的归一化。
layout(push_constant) uniform Push {
    mat4 mvp;          // 像素坐标 → NDC
    vec4 effectRect;   // 目标矩形 x,y,w,h（视觉像素）
    vec4 effectParams; // 各管线自定义（blur 的采样步长等），本阶段不用
    vec4 effectExtra;  // xy = texelSize（1/纹理宽, 1/纹理高）
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUv;
layout(location = 2) out vec2 fragPos;

void main() {
    // 单位四边形两三角形：(0,0)(1,0)(1,1) / (0,0)(1,1)(0,1)。
    vec2 quad[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                          vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 pos01 = quad[gl_VertexIndex];
    vec2 pixel = pc.effectRect.xy + pos01 * pc.effectRect.zw;
    gl_Position = pc.mvp * vec4(pixel, 0.0, 1.0);
    fragUv = pixel * pc.effectExtra.xy;
    fragPos = pixel;
    fragColor = vec4(1.0);
}
