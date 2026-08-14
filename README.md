# estarx_vulkan

A minimal, self-contained 2D rendering framework extracted from `estarx`.
It separates platform-agnostic code from platform-specific shells,
with Android (Vulkan) supported.

> 📘 **中文文档**：详见 `文档/README.md`（索引）、`文档/构建指南.md`、`文档/架构设计.md`、`文档/示例应用导读.md`、`文档/源码导读.md`。

![Screenshot](estarx_vulkan_screenshot.png)

## Structure

```
estarx_vulkan/
├── core/                        # Platform-independent core (C++17)
│   ├── include/evk/
│   │   ├── renderer.h           # evk::Renderer public API
│   │   ├── platform.h           # IPlatform abstraction
│   │   ├── event.h              # App lifecycle callback API
│   │   ├── log.h                # spdlog-based logging
│   │   ├── render_loop.h        # Dirty frame + platform VSync API
│   │   ├── esx_view.h           # Handle-based view ABI (esx_view)
│   │   └── ui/
│   │       ├── view.h           # View tree (layout/callbacks/hitTest)
│   │       ├── input.h          # Cross-platform pointer dispatch
│   │       ├── animator.h       # VSync-driven per-frame animation
│   │       ├── controls/        # SDK Button / ScrollView / Navigation controls
│   │       └── canvas.h         # Per-frame vertex canvas + clip stack
│   ├── src/
│   │   ├── renderer.cpp         # Vulkan pipeline; renders a Canvas
│   │   ├── event.cpp            # Event dispatch implementation
│   │   ├── render_loop.cpp      # Dirty frame coalescing
│   │   ├── esx_view.cpp         # View ABI + frame build
│   │   ├── ui/view.cpp          # View tree implementation
│   │   ├── ui/input.cpp         # Hit test + click/pan dispatch
│   │   ├── ui/animator.cpp      # Animation tick driver
│   │   ├── ui/controls/         # Standard control implementation
│   │   └── ui/canvas.cpp        # Canvas implementation
│   └── shaders/
│       ├── ui.vert              # UI vertex shader (push-constant ortho MVP)
│       └── ui.frag              # UI fragment shader (vertex color)
├── platform/android/            # Android JNI bridge + SurfaceView
│   ├── cpp/bridge.cpp
│   ├── cpp/platform_android.cpp # Android platform implementation
│   └── java/com/estarx/vulkan/
├── samples/app/                 # Shared cross-platform App C++ sources
├── samples/android/             # Android-only entry/build/resources
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
       path/to/platform/android/cpp/platform_android.cpp
       path/to/core/src/renderer.cpp
       path/to/core/src/event.cpp
       path/to/core/src/render_loop.cpp
       path/to/core/src/esx_view.cpp
       path/to/core/src/ui/view.cpp
       path/to/core/src/ui/input.cpp
       path/to/core/src/ui/animator.cpp
       path/to/core/src/ui/canvas.cpp
       path/to/core/src/ui/controls/button.cpp
       path/to/core/src/ui/controls/scroll_view.cpp
       path/to/core/src/ui/controls/navigation.cpp
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
