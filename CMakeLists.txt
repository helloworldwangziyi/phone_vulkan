cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_UI_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui.vert
            -DFRAG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui.frag
            -DOUTPUT_HEADER=${EVK_UI_SHADER_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# Core Vulkan renderer (platform-independent).
add_library(estarx_vulkan_core STATIC
    core/src/event.cpp
    core/src/render_loop.cpp
    core/src/renderer.cpp
    core/src/esx_view.cpp
    core/src/ui/view.cpp
    core/src/ui/input.cpp
    core/src/ui/animator.cpp
    core/src/ui/canvas.cpp
    core/src/ui/controls/button.cpp
    core/src/ui/controls/scroll_view.cpp
    core/src/ui/controls/navigation.cpp
)

target_include_directories(estarx_vulkan_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm
    PRIVATE
        ${EVK_GENERATED_DIR}
)

if (ANDROID)
    # renderer.cpp / platform_android.cpp 用到 VK_KHR_android_surface 扩展的类型与常量，
    # 必须在包含 vulkan.h 前定义该宏才可见。
    # （sample 工程的 CMakeLists 独立编译源码、不经由本文件，仍需各自定义。）
    add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)
    find_library(vulkan-lib vulkan)
    target_link_libraries(estarx_vulkan_core PUBLIC ${vulkan-lib})
else()
    find_package(Vulkan REQUIRED)
    target_link_libraries(estarx_vulkan_core PUBLIC Vulkan::Vulkan)
endif()

# Android platform layer.
if (ANDROID)
    add_library(estarx_vulkan_android STATIC
        platform/android/cpp/platform_android.cpp
    )

    target_include_directories(estarx_vulkan_android PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    )

    find_library(android-lib android)
    find_library(log-lib log)

    target_link_libraries(estarx_vulkan_android PUBLIC
        estarx_vulkan_core
        ${android-lib}
        ${log-lib}
    )
endif()
