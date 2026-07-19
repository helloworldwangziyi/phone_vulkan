# estarx_vulkan

A minimal, self-contained Vulkan rendering framework extracted from `estarx`.
It separates platform-agnostic rendering code from platform-specific surface
management, with Android as the first supported platform.

> 📘 **中文学习文档**：详见 `文档/README.md`、`文档/架构设计.md`、`文档/源码导读.md`、`文档/构建指南.md`。

![Screenshot](estarx_vulkan_screenshot.png)

## Structure

```
estarx_vulkan/
├── core/                        # Platform-independent Vulkan renderer
│   ├── include/evk/
│   │   ├── renderer.h           # evk::Renderer public API
│   │   └── platform.h           # IPlatform abstraction
│   ├── src/
│   │   ├── renderer.cpp         # Full Vulkan pipeline (triangle demo)
│   │   └── platform_android.cpp # Android platform implementation
│   └── shaders/
│       ├── triangle.vert
│       └── triangle.frag
├── platform/android/            # Android JNI bridge + SurfaceView
│   ├── cpp/bridge.cpp
│   └── java/com/estarx/vulkan/
├── samples/android/             # Runnable Android sample app
└── CMakeLists.txt               # Framework-level CMake
```

## Build the Android sample

```bash
cd estarx_vulkan
./gradlew :samples:android:app:assembleDebug
```

The APK is produced at:
`samples/android/app/build/outputs/apk/debug/app-debug.apk`

## Use the framework in your own Android project

1. Copy `core/` and `platform/android/` into your project.
2. Add the native sources to your `CMakeLists.txt`:
   ```cmake
   add_library(my_vulkan SHARED
       path/to/platform/android/cpp/bridge.cpp
       path/to/core/src/platform_android.cpp
       path/to/core/src/renderer.cpp
   )
   target_include_directories(my_vulkan PUBLIC path/to/core/include)
   target_link_libraries(my_vulkan log android vulkan)
   ```
3. Copy `NativeBridge.java` and `VulkanSurfaceView.java` to your Java package.
4. Place `VulkanSurfaceView` in your layout.

## Extending to other platforms

Implement `evk::IPlatform` for your windowing system (GLFW, Win32, etc.) and
construct `evk::Renderer` with it. Only `createVulkanSurface`, `getSurfaceSize`
and `log` need to be provided.
