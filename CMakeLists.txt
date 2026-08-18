cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_UI_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui_vertex.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui_fragment.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ui_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui_vertex.vert
            -DFRAG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${EVK_UI_SHADER_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ui_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# 字体资产：二进制内嵌为字节数组（FontEngine 注册用，同 shader 的免运行时读文件方案）。
set(EVK_FONT_HEADER "${EVK_GENERATED_DIR}/evk/assets/font_assets.h")
set(EVK_FONT_LIST
    "kFontRobotoRegular=${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/roboto_regular_subset.ttf"
    "kFontNotoSansScRegular=${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/notosanssc_regular_subset.ttf"
    "kFontNotoSansScBold=${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/notosanssc_bold_subset.ttf"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/roboto_regular_subset.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/notosanssc_regular_subset.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/assets/fonts/notosanssc_bold_subset.ttf"
)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DFONT_LIST=${EVK_FONT_LIST}"
            -DOUTPUT_HEADER=${EVK_FONT_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font_assets.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# Core Vulkan renderer (platform-independent).
add_library(estarx_vulkan_core STATIC
    core/src/app_lifecycle.cpp
    core/src/frame_scheduler.cpp
    core/src/vulkan_renderer.cpp
    core/src/ui/ui_application.cpp
    core/src/ui/render_view.cpp
    core/src/ui/pointer_input.cpp
    core/src/ui/animation_scheduler.cpp
    core/src/ui/paint_canvas.cpp
    core/src/ui/event_bus.cpp
    core/src/ui/font_engine.cpp
    core/src/ui/texture_store.cpp
    core/src/ui/view/button_control.cpp
    core/src/ui/layout/flex_layout.cpp
    core/src/ui/view/scroll_control.cpp
    core/src/ui/navigation/navigation_stack.cpp
    core/src/ui/widget_tree.cpp
    core/src/ui/controls/basic.cpp
    core/src/ui/controls/button.cpp
    core/src/ui/controls/container.cpp
    core/src/ui/controls/flex.cpp
    core/src/ui/controls/image.cpp
    core/src/ui/controls/list_view.cpp
    core/src/ui/controls/scroll_view.cpp
    core/src/ui/controls/text.cpp
)

target_include_directories(estarx_vulkan_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm
    PRIVATE
        ${EVK_GENERATED_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

if (ANDROID)
    # vulkan_renderer.cpp / android_vulkan_platform.cpp 用到 VK_KHR_android_surface 扩展的类型与常量，
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
        platform/android/cpp/android_vulkan_platform.cpp
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
