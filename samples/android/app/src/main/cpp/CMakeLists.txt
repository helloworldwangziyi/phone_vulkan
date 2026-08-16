cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EVK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_UI_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/zy_ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EVK_ROOT}/core/shaders/zy_ui_vertex.vert"
    "${EVK_ROOT}/core/shaders/zy_ui_fragment.frag"
    "${EVK_ROOT}/cmake/zy_embed_ui_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=zy_ui
            -DVERT_SRC=${EVK_ROOT}/core/shaders/zy_ui_vertex.vert
            -DFRAG_SRC=${EVK_ROOT}/core/shaders/zy_ui_fragment.frag
            -DOUTPUT_HEADER=${EVK_UI_SHADER_HEADER}
            -P "${EVK_ROOT}/cmake/zy_embed_ui_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/samples/app/zy_app_entry.cpp
    ${EVK_ROOT}/samples/app/zy_screen_metrics.cpp
    ${EVK_ROOT}/samples/app/zy_app_theme.cpp
    ${EVK_ROOT}/samples/app/zy_home_page.cpp
    ${EVK_ROOT}/samples/app/zy_detail_page.cpp
    ${EVK_ROOT}/platform/android/cpp/zy_android_jni_bridge.cpp
    ${EVK_ROOT}/platform/android/cpp/zy_android_vulkan_platform.cpp
    ${EVK_ROOT}/core/src/zy_app_lifecycle.cpp
    ${EVK_ROOT}/core/src/zy_frame_scheduler.cpp
    ${EVK_ROOT}/core/src/zy_vulkan_renderer.cpp
    ${EVK_ROOT}/core/src/ui/zy_ui_application.cpp
    ${EVK_ROOT}/core/src/ui/zy_render_view.cpp
    ${EVK_ROOT}/core/src/ui/zy_pointer_input.cpp
    ${EVK_ROOT}/core/src/ui/zy_animation_scheduler.cpp
    ${EVK_ROOT}/core/src/ui/zy_paint_canvas.cpp
    ${EVK_ROOT}/core/src/ui/zy_event_bus.cpp
    ${EVK_ROOT}/core/src/ui/controls/zy_button_control.cpp
    ${EVK_ROOT}/core/src/ui/layout/zy_flex_layout.cpp
    ${EVK_ROOT}/core/src/ui/controls/zy_scroll_control.cpp
    ${EVK_ROOT}/core/src/ui/navigation/zy_navigation_stack.cpp
    ${EVK_ROOT}/core/src/ui/zy_widget_tree.cpp
)

target_include_directories(estarx_vulkan
    PUBLIC
        ${EVK_ROOT}/core/include
        ${EVK_ROOT}/samples/app
        ${EVK_ROOT}/third_party/spdlog/include
        ${EVK_ROOT}/third_party/glm
    PRIVATE
        ${EVK_GENERATED_DIR}
)

if (ANDROID)
    target_compile_definitions(estarx_vulkan PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_link_libraries(estarx_vulkan
    ${log-lib}
    ${android-lib}
    ${vulkan-lib}
)
