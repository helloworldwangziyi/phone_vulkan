cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EVK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/platform/android/cpp/bridge.cpp
    ${EVK_ROOT}/core/src/platform_android.cpp
    ${EVK_ROOT}/core/src/renderer.cpp
)

target_include_directories(estarx_vulkan PUBLIC
    ${EVK_ROOT}/core/include
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
