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
    "${EVK_ROOT}/core/shaders/ui.vert"
    "${EVK_ROOT}/core/shaders/ui.frag"
    "${EVK_ROOT}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${EVK_ROOT}/core/shaders/ui.vert
            -DFRAG_SRC=${EVK_ROOT}/core/shaders/ui.frag
            -DOUTPUT_HEADER=${EVK_UI_SHADER_HEADER}
            -P "${EVK_ROOT}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/samples/app/app_main.cpp
    ${EVK_ROOT}/samples/app/app_metrics.cpp
    ${EVK_ROOT}/samples/app/app_theme.cpp
    ${EVK_ROOT}/samples/app/main_page.cpp
    ${EVK_ROOT}/samples/app/detail_page.cpp
    ${EVK_ROOT}/platform/android/cpp/bridge.cpp
    ${EVK_ROOT}/platform/android/cpp/platform_android.cpp
    ${EVK_ROOT}/core/src/event.cpp
    ${EVK_ROOT}/core/src/render_loop.cpp
    ${EVK_ROOT}/core/src/renderer.cpp
    ${EVK_ROOT}/core/src/esx_view.cpp
    ${EVK_ROOT}/core/src/ui/view.cpp
    ${EVK_ROOT}/core/src/ui/input.cpp
    ${EVK_ROOT}/core/src/ui/animator.cpp
    ${EVK_ROOT}/core/src/ui/canvas.cpp
    ${EVK_ROOT}/core/src/ui/event_bus.cpp
    ${EVK_ROOT}/core/src/ui/controls/button.cpp
    ${EVK_ROOT}/core/src/ui/controls/flex.cpp
    ${EVK_ROOT}/core/src/ui/controls/scroll_view.cpp
    ${EVK_ROOT}/core/src/ui/controls/navigation.cpp
    ${EVK_ROOT}/core/src/ui/widget.cpp
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
