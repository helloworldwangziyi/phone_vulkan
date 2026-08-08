cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EVK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_TRIANGLE_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/triangle_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EVK_ROOT}/core/shaders/triangle.vert"
    "${EVK_ROOT}/core/shaders/triangle.frag"
    "${EVK_ROOT}/cmake/embed_triangle_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DVERT_SRC=${EVK_ROOT}/core/shaders/triangle.vert
            -DFRAG_SRC=${EVK_ROOT}/core/shaders/triangle.frag
            -DOUTPUT_HEADER=${EVK_TRIANGLE_SHADER_HEADER}
            -P "${EVK_ROOT}/cmake/embed_triangle_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/platform/android/cpp/bridge.cpp
    ${EVK_ROOT}/core/src/event.cpp
    ${EVK_ROOT}/core/src/platform_android.cpp
    ${EVK_ROOT}/core/src/renderer.cpp
)

target_include_directories(estarx_vulkan
    PUBLIC
        ${EVK_ROOT}/core/include
        ${EVK_ROOT}/third_party/spdlog/include
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
