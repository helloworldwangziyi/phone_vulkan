#include "evk/render_loop.h"

namespace {

evk::FrameFunc g_frameFunc = nullptr;

} // namespace

namespace evk {

void setFrameFunc(FrameFunc func) {
    g_frameFunc = func;
}

void requestRender() {
    // 按需模型：请求即画。未注册时兜底忽略。
    if (g_frameFunc) {
        g_frameFunc();
    }
}

} // namespace evk
