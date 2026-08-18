cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    ${EVK_ROOT}/platform/android/cpp/android_jni_bridge.cpp
    ${EVK_ROOT}/platform/android/cpp/android_vulkan_platform.cpp
    ${EVK_ROOT}/core/src/app_lifecycle.cpp
    ${EVK_ROOT}/core/src/frame_scheduler.cpp
    ${EVK_ROOT}/core/src/kv_store.cpp
    ${EVK_ROOT}/core/src/vulkan_renderer.cpp
    ${EVK_ROOT}/core/src/ui/ui_application.cpp
    ${EVK_ROOT}/core/src/ui/render_view.cpp
    ${EVK_ROOT}/core/src/ui/pointer_input.cpp
    ${EVK_ROOT}/core/src/ui/animation_scheduler.cpp
    ${EVK_ROOT}/core/src/ui/paint_canvas.cpp
    ${EVK_ROOT}/core/src/ui/event_bus.cpp
    ${EVK_ROOT}/core/src/ui/font_engine.cpp
    ${EVK_ROOT}/core/src/ui/texture_store.cpp
    ${EVK_ROOT}/core/src/ui/controls/button_control.cpp
    ${EVK_ROOT}/core/src/ui/layout/flex_layout.cpp
    ${EVK_ROOT}/core/src/ui/controls/scroll_control.cpp
    ${EVK_ROOT}/core/src/ui/navigation/navigation_stack.cpp
    ${EVK_ROOT}/core/src/ui/widget_tree.cpp
)

# MMKV Core（键值持久化，KeyValueStore 的实现）：静态库 target `core`，
# 其 CMakeLists 自带 Android 分支（ashmem、armv8+crypto 汇编、链接 libz）。
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
        # kv_store.cpp 直接 include "MMKV.h"（与 iOS 手编配置同款约定）。
        ${EVK_ROOT}/third_party/mmkv/Core
)

if (ANDROID)
    target_compile_definitions(estarx_vulkan PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_link_libraries(estarx_vulkan
    core
    ${log-lib}
    ${android-lib}
    ${vulkan-lib}
)
