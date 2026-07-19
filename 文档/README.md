# estarx_vulkan 项目总览

## 简介

`estarx_vulkan` 是一个**最小化的跨平台 Vulkan 渲染框架**，目标是把 Vulkan 渲染核心与平台相关的窗口/表面管理解耦，让同一份 C++ 渲染代码可以同时运行在 Android、桌面（Windows/Linux/macOS）等平台上。

本项目目前以 **Android** 为第一支持平台，桌面平台已通过 CMake + Visual Studio 验证可编译。整个框架从 `estarx` 主项目中提取出来，保留了最精简、最易于学习的结构。

![运行截图](../estarx_vulkan_screenshot.png)

> 上图是在 Android 模拟器（API 36）中运行的效果：一个红/绿/蓝三顶点的彩色三角形，背景为深色。

## 项目特点

- **平台无关核心**：`evk::Renderer` 不依赖任何平台 API，只通过 `evk::IPlatform` 接口与外部交互。
- **真正的 Vulkan 实现**：包含完整的 Instance / Surface / Device / Swapchain / RenderPass / Pipeline / CommandBuffer / Sync Objects 初始化流程。
- **自包含 Shader**：顶点/片段 Shader 已预编译成 SPIR-V 并内嵌在 C++ 源码中，无需运行时依赖 `glslc`。
- **Android 即开即用**：提供 `VulkanSurfaceView` + `NativeBridge`，直接拖进布局即可使用。
- **可扩展**：实现一个新的 `IPlatform` 即可把核心渲染器移植到 GLFW、Win32、Wayland 等窗口系统。

## 目录结构

```text
estarx_vulkan/
├── core/                          # 平台无关 Vulkan 核心
│   ├── include/evk/
│   │   ├── platform.h             # IPlatform 抽象接口
│   │   └── renderer.h             # evk::Renderer 公共 API
│   ├── src/
│   │   ├── renderer.cpp           # Vulkan 初始化与绘制实现
│   │   └── platform_android.cpp   # Android 平台实现
│   └── shaders/                   # GLSL 源文件（已内嵌 SPIR-V）
│       ├── triangle.vert
│       └── triangle.frag
├── platform/android/              # Android 平台层
│   ├── cpp/bridge.cpp             # JNI 桥接层
│   └── java/com/estarx/vulkan/
│       ├── NativeBridge.java      # Java native 方法声明
│       └── VulkanSurfaceView.java # 封装 SurfaceView
├── samples/android/               # Android 示例应用
│   └── app/
│       ├── build.gradle
│       └── src/main/
│           ├── AndroidManifest.xml
│           ├── cpp/CMakeLists.txt
│           ├── java/com/estarx/vulkansample/MainActivity.java
│           └── res/
├── 文档/                           # 项目文档（本目录）
├── CMakeLists.txt                 # 顶层 CMake（桌面 / Android）
├── build.gradle / settings.gradle / gradle.properties
├── gradlew / gradlew.bat / gradle/wrapper/
└── README.md                      # 顶层快速开始说明
```

## 快速开始

### 构建 Android 示例

需要：JDK 17+、Android SDK（API 34）、Android NDK。

```bash
cd estarx_vulkan
./gradlew :samples:android:app:assembleDebug
```

构建产物：

```text
samples/android/app/build/outputs/apk/debug/app-debug.apk
```

安装并运行：

```bash
./gradlew :samples:android:app:installDebug
adb shell am start -n com.estarx.vulkansample/.MainActivity
```

### 桌面编译（仅验证 C++ 核心）

需要：CMake 3.22+、Vulkan SDK。

```bash
cd estarx_vulkan
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

## 学习路线建议

1. 先读 `文档/架构设计.md`，理解为什么要把项目分成 core / platform / sample 三层。
2. 再读 `文档/源码导读.md`，跟着代码走一遍 Vulkan 初始化到绘制的完整流程。
3. 最后读 `文档/构建指南.md`，在自己的机器上把项目跑起来，并尝试改顶点数据、颜色、背景色。

## 许可证与来源

本项目从 `estarx` 主项目的 `vulkan_app/` 模块中提取并改写。原模块仅包含空壳代码，本项目补齐了真正的 Vulkan 渲染实现。
