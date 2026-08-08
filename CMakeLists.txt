cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_TRIANGLE_SHADER_HEADER "${EVK_GENERATED_DIR}/evk/assets/triangle_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/evk/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/triangle.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/triangle.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_triangle_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DVERT_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/triangle.vert
            -DFRAG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/triangle.frag
            -DOUTPUT_HEADER=${EVK_TRIANGLE_SHADER_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_triangle_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# Core Vulkan renderer (platform-independent).
add_library(estarx_vulkan_core STATIC
    core/src/event.cpp
    core/src/renderer.cpp
)

target_include_directories(estarx_vulkan_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
    PRIVATE
        ${EVK_GENERATED_DIR}
)

if (ANDROID)
    find_library(vulkan-lib vulkan)
    target_link_libraries(estarx_vulkan_core PUBLIC ${vulkan-lib})
else()
    find_package(Vulkan REQUIRED)
    target_link_libraries(estarx_vulkan_core PUBLIC Vulkan::Vulkan)
endif()

# Android platform layer.
if (ANDROID)
    add_library(estarx_vulkan_android STATIC
        core/src/platform_android.cpp
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
