cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core Vulkan renderer (platform-independent).
add_library(estarx_vulkan_core STATIC
    core/src/event.cpp
    core/src/renderer.cpp
)

target_include_directories(estarx_vulkan_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
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
