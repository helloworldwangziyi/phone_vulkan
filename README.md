# estarx_vulkan

A minimal, self-contained Vulkan rendering framework extracted from `estarx`.
It separates platform-agnostic rendering code from platform-specific surface
management, with Android as the first supported platform.

> 📘 **中文文档**：详见 `文档/README.md`（索引）、`文档/构建指南.md`、`文档/架构设计.md`、`文档/源码导读.md`。

![Screenshot](estarx_vulkan_screenshot.png)

## Structure

```
estarx_vulkan/
├── core/                        # Platform-independent core (C++17)
│   ├── include/evk/
│   │   ├── renderer.h           # evk::Renderer public API
│   │   ├── platform.h           # IPlatform abstraction
│   │   ├── event.h              # Cross-platform event dispatch API
│   │   ├── log.h                # spdlog-based logging
│   │   ├── render_loop.h        # requestRender() / setFrameFunc()
│   │   ├── esx_view.h           # Handle-based view ABI (esx_view)
│   │   └── ui/
│   │       ├── view.h           # View tree (rect/visible/children/hitTest)
│   │       └── canvas.h         # Per-frame vertex canvas + clip stack
│   ├── src/
│   │   ├── renderer.cpp         # Vulkan pipeline; renders a Canvas
│   │   ├── event.cpp            # Event dispatch implementation
│   │   ├── render_loop.cpp      # Frame-func registry
│   │   ├── esx_view.cpp         # View ABI + frame build + touch dispatch
│   │   ├── ui/view.cpp          # View tree implementation
│   │   ├── ui/canvas.cpp        # Canvas implementation
│   │   └── platform_android.cpp # Android platform implementation
│   └── shaders/
│       ├── ui.vert              # UI vertex shader (push-constant ortho MVP)
│       └── ui.frag              # UI fragment shader (vertex color)
├── platform/android/            # Android JNI bridge + SurfaceView
│   ├── cpp/bridge.cpp
│   └── java/com/estarx/vulkan/
├── samples/android/             # Runnable Android sample app
│   └── app/src/main/cpp/app_main.cpp  # Sample: builds views, handles events
├── third_party/
│   ├── spdlog/                  # Header-only logging library
│   └── glm/                     # Header-only math library (matrix helpers)
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
       path/to/core/src/event.cpp
       path/to/core/src/render_loop.cpp
       path/to/core/src/esx_view.cpp
       path/to/core/src/ui/view.cpp
       path/to/core/src/ui/canvas.cpp
   )
   target_include_directories(my_vulkan PUBLIC
       path/to/core/include
       path/to/third_party/spdlog/include
       path/to/third_party/glm
   )
   target_link_libraries(my_vulkan log android vulkan)
   ```
3. Copy `NativeBridge.java` and `VulkanSurfaceView.java` to your Java package.
4. Place `VulkanSurfaceView` in your layout.

## Extending to other platforms

Implement `evk::IPlatform` for your windowing system (GLFW, Win32, etc.) and
construct `evk::Renderer` with it. Only `createVulkanSurface`, `getSurfaceSize`
and `log` need to be provided.
