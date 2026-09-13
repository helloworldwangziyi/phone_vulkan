#version 450

// SDF 片元着色器：基础公式仍是"纹理 × 顶点色"，再乘一个由圆角矩形
// 距离场推出的覆盖率 alpha：
//   mode 0（阴影）：形内不衰减（由自身不透明背景遮盖），形外按 blur
//     窗口 smoothstep 羽化——对照 Impeller 的 RRect 阴影 SDF 羽化；
//   mode 1（圆角裁剪）：形内 1、形外 0，边缘 1px smoothstep 抗锯齿，
//     内容（含文字/图像）原样采样，只被圆角遮罩削角。
// 参数经 push constant 按批下发（effectRect = 形状矩形，
// effectParams = radius, blur, mode, pad）。
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUv;
layout(location = 2) in vec2 fragPos;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 effectRect;
    vec4 effectParams;
} pc;

layout(location = 0) out vec4 outColor;

// 圆角矩形有向距离场（<0 形内，0 边界，>0 形外）。
float sdRoundBox(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

void main() {
    vec4 sampled = texture(uTexture, fragUv);
    vec4 base = vec4(fragColor.rgb * sampled.rgb, fragColor.a * sampled.a);

    vec2 halfSize = pc.effectRect.zw * 0.5;
    vec2 center = pc.effectRect.xy + halfSize;
    float radius = min(pc.effectParams.x, min(halfSize.x, halfSize.y));
    float d = sdRoundBox(fragPos - center, halfSize, max(radius, 0.0));

    float alpha;
    if (pc.effectParams.z < 0.5) {
        // mode 0：阴影羽化（窗口至少 1px，blur=0 时是硬边阴影）
        float soft = max(pc.effectParams.y, 1.0);
        alpha = 1.0 - smoothstep(0.0, soft, d);
    } else if (pc.effectParams.z < 1.5) {
        // mode 1：圆角裁剪，1px 抗锯齿边缘
        alpha = 1.0 - smoothstep(-0.5, 0.5, d);
    } else {
        // mode 2：模糊合成专用——过渡带外移 1px（区域外 ping 图仍有有效
        // 模糊内容，带内不漏锐利场景，滚动时卡边缘不出现「重影」亮线）。
        alpha = 1.0 - smoothstep(0.5, 1.5, d);
    }
    outColor = vec4(base.rgb, base.a * alpha);
}
