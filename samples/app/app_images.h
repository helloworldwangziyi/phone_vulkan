#pragma once

/**
 * @file app_images.h
 * @brief App 位图资产：演示 TextureStore 的动态图像登记（无二进制资源文件）。
 *
 * 真实项目里这里通常是解码后的 PNG/JPEG 像素；示例用程序化生成的
 * 径向渐变徽章展示同一条 drawImage 通道。
 */
#include "evk/ui/texture_store.h"

namespace appImages {

/// 径向渐变徽章（128x128 RGBA，青→靛蓝、圆形软边）。
evk::ui::TextureId badge();

/// 首次使用前注册（幂等；EngineReady 后调用一次即可）。
void ensureRegistered();

} // namespace appImages
