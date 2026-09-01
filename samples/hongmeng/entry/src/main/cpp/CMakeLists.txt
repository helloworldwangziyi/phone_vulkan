cmake_minimum_required(VERSION 3.16)

# 隐式启用 C 与 CXX：third_party/libunibreak（UAX #14 断行）为纯 C 源码。
project(estarx_vulkan_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 与 samples/android 同款：从 cpp 目录回溯 6 级到仓库根。
set(EVK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_UI_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EVK_ROOT}/core/shaders/ui_vertex.vert"
    "${EVK_ROOT}/core/shaders/ui_fragment.frag"
    "${EVK_ROOT}/cmake/embed_ui_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${EVK_ROOT}/core/shaders/ui_vertex.vert
            -DFRAG_SRC=${EVK_ROOT}/core/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${EVK_UI_SHADER_HEADER}
            -P "${EVK_ROOT}/cmake/embed_ui_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# 字体资产：二进制内嵌为字节数组（FontEngine 注册用）。
set(EVK_FONT_HEADER "${EVK_GENERATED_DIR}/evk/assets/font_assets.h")
set(EVK_FONT_LIST
    "kFontRobotoRegular=${EVK_ROOT}/core/assets/fonts/roboto_regular_subset.ttf"
    "kFontNotoSansScRegular=${EVK_ROOT}/core/assets/fonts/notosanssc_regular_subset.ttf"
    "kFontNotoSansScBold=${EVK_ROOT}/core/assets/fonts/notosanssc_bold_subset.ttf"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EVK_ROOT}/core/assets/fonts/roboto_regular_subset.ttf"
    "${EVK_ROOT}/core/assets/fonts/notosanssc_regular_subset.ttf"
    "${EVK_ROOT}/core/assets/fonts/notosanssc_bold_subset.ttf"
)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DFONT_LIST=${EVK_FONT_LIST}"
            -DOUTPUT_HEADER=${EVK_FONT_HEADER}
            -P "${EVK_ROOT}/cmake/embed_font_assets.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/samples/app/app_entry.cpp
    ${EVK_ROOT}/samples/app/app_fonts.cpp
    ${EVK_ROOT}/samples/app/app_images.cpp
    ${EVK_ROOT}/samples/app/screen_metrics.cpp
    ${EVK_ROOT}/samples/app/app_theme.cpp
    ${EVK_ROOT}/samples/app/home_page.cpp
    ${EVK_ROOT}/samples/app/detail_page.cpp
    ${EVK_ROOT}/samples/app/watchlist_page.cpp
    ${EVK_ROOT}/samples/app/path_demo_page.cpp
    ${EVK_ROOT}/platform/hongmeng/cpp/hongmeng_napi_bridge.cpp
    ${EVK_ROOT}/platform/hongmeng/cpp/hongmeng_vulkan_platform.cpp
    ${EVK_ROOT}/core/src/app_lifecycle.cpp
    ${EVK_ROOT}/core/src/frame_scheduler.cpp
    ${EVK_ROOT}/core/src/kv_store.cpp
    ${EVK_ROOT}/core/src/compositor.cpp
    ${EVK_ROOT}/core/src/vulkan_renderer.cpp
    ${EVK_ROOT}/core/src/gpu/vulkan_context.cpp
    ${EVK_ROOT}/core/src/gpu/swapchain.cpp
    ${EVK_ROOT}/core/src/gpu/ui_pipeline.cpp
    ${EVK_ROOT}/core/src/gpu/texture_cache.cpp
    ${EVK_ROOT}/core/src/ui/ui_application.cpp
    ${EVK_ROOT}/core/src/ui/render_view.cpp
    ${EVK_ROOT}/core/src/ui/pointer_input.cpp
    ${EVK_ROOT}/core/src/ui/animation_scheduler.cpp
    ${EVK_ROOT}/core/src/ui/paint_canvas.cpp
    ${EVK_ROOT}/core/src/ui/event_bus.cpp
    ${EVK_ROOT}/core/src/ui/font_engine.cpp
    ${EVK_ROOT}/core/src/ui/text_layout.cpp
    ${EVK_ROOT}/core/src/ui/texture_store.cpp
    ${EVK_ROOT}/core/src/ui/path.cpp
    ${EVK_ROOT}/core/src/ui/view/button_control.cpp
    ${EVK_ROOT}/core/src/ui/layout/flex_layout.cpp
    ${EVK_ROOT}/core/src/ui/view/scroll_control.cpp
    ${EVK_ROOT}/core/src/ui/navigation/navigation_stack.cpp
    ${EVK_ROOT}/core/src/ui/widget_tree.cpp
    ${EVK_ROOT}/core/src/ui/controls/basic.cpp
    ${EVK_ROOT}/core/src/ui/controls/button.cpp
    ${EVK_ROOT}/core/src/ui/controls/container.cpp
    ${EVK_ROOT}/core/src/ui/controls/flex.cpp
    ${EVK_ROOT}/core/src/ui/controls/image.cpp
    ${EVK_ROOT}/core/src/ui/controls/list_view.cpp
    ${EVK_ROOT}/core/src/ui/controls/scroll_view.cpp
    ${EVK_ROOT}/core/src/ui/controls/text.cpp
    # libunibreak：UAX #14 断行（text_layout.cpp 用），纯 C。
    ${EVK_ROOT}/third_party/libunibreak/linebreak.c
    ${EVK_ROOT}/third_party/libunibreak/linebreakdata.c
    ${EVK_ROOT}/third_party/libunibreak/linebreakdef.c
    ${EVK_ROOT}/third_party/libunibreak/unibreakbase.c
    ${EVK_ROOT}/third_party/libunibreak/unibreakdef.c
    ${EVK_ROOT}/third_party/libunibreak/eastasianwidthdata.c
    ${EVK_ROOT}/third_party/libunibreak/eastasianwidthdef.c
)

# MMKV Core（键值持久化，KeyValueStore 的实现）：静态库 target `core`，
# 其 CMakeLists 自带 OHOS 分支（armv8+crypto 汇编、libz 缺失时自动内嵌 zlib）。
add_subdirectory(${EVK_ROOT}/third_party/mmkv/Core ${CMAKE_CURRENT_BINARY_DIR}/mmkv_core)

target_include_directories(estarx_vulkan
    PUBLIC
        ${EVK_ROOT}/core/include
        ${EVK_ROOT}/samples/app
        ${EVK_ROOT}/third_party/spdlog/include
        ${EVK_ROOT}/third_party/glm
    PRIVATE
        ${EVK_GENERATED_DIR}
        ${EVK_ROOT}/third_party/stb
        ${EVK_ROOT}/third_party/libunibreak
        # kv_store.cpp 直接 include "MMKV.h"（与 Android 配置同款约定）。
        ${EVK_ROOT}/third_party/mmkv/Core
)

if (OHOS)
    # vulkan.h 依此宏包含 vulkan_ohos.h（vkCreateSurfaceOHOS 等声明随之可见）。
    target_compile_definitions(estarx_vulkan PRIVATE VK_USE_PLATFORM_OHOS=1)
endif()

find_library(ace-ndk-lib ace_ndk.z)       # XComponent NDK（OH_NativeXComponent_*）
find_library(ace-napi-lib ace_napi.z)     # NAPI 运行时
find_library(native-window-lib native_window)
find_library(hilog-lib hilog_ndk.z)       # OH_LOG_Print（日志 sink）
find_library(vulkan-lib vulkan)

target_link_libraries(estarx_vulkan
    core
    ${ace-ndk-lib}
    ${ace-napi-lib}
    ${native-window-lib}
    ${hilog-lib}
    ${vulkan-lib}
)
