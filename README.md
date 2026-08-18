# estarx_vulkan

A small cross-platform C++17 UI framework rendered by Vulkan. The application
API follows Flutter's retained declarative model and is fully C++: there are no
view handles, C callbacks, user-data pointers, or public C ABI layers.

Chinese documentation is indexed in 文档/README.md.

## UI model

The framework keeps three separate trees and responsibilities:

- Widget: immutable configuration rebuilt by application code.
- Element: mounted identity, reconciliation, BuildContext, and State ownership.
- View: retained render object used internally for layout, paint, and hit testing.

Stateful pages use StatefulWidget plus State. Layout is composed with Column,
Row, Padding, SizedBox, Center, and Expanded. Routes use
Navigator::of(context).push(...) and are owned by the Navigator.

~~~cpp
class CounterPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};

class CounterState final : public evk::ui::State {
public:
    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext& context) override {
        using namespace evk::ui;
        return column(
            expanded(container(0x1E293BFF)),
            center(sizedBox(
                320.0f,
                96.0f,
                button({}, [&context] {
                    Navigator::of(context).push(
                        makeWidget<DetailPage>(), true);
                }))));
    }
};
~~~

## Structure

~~~
core/
  include/evk/ui/
    ui_application.h   runApp, shutdownApp, viewport
    widget_tree.h      Widget, Element, State (framework core)
    widgets.h          umbrella header: framework + all widgets
    render_view.h      internal retained render-object tree
    pointer_input.h    pointer dispatch and gesture ownership
    event_bus.h        C++ RAII subscriptions and UI-thread queue
    font_engine.h      TTF parsing, glyph atlas, and font fallback
    texture_store.h     unified RGBA texture registry (atlas + images)
    controls/          widget layer (descriptions): container, text, image,
      basic.h           button, flex (Column/Row), scroll_view, list_view,
                        and basic.h for EdgeInsets/SizedBox/Padding/...
    view/               view layer (retained render objects)
      button_control.h Button render object
      scroll_control.h ScrollView/ListView render object
    layout/
      flex_layout.h    Row and Column layout capability
    navigation/
      navigation_stack.h Navigator and route transitions
  src/ui/                 implementations of the modules above
  assets/fonts/           embedded font binaries (Roboto + Noto Sans SC, OFL)
  shaders/                UI vertex/fragment shaders (texture x vertex color)
platform/
  android/                JNI and Vulkan surface shell
  ios/                    Objective-C++ and MoltenVK shell
samples/app/              shared Flutter-style C++ application
tests/                    runtime contract tests
~~~

Project-owned source files use plain `<responsibility>` names such as
`widget_tree.h` and `android_jni_bridge.cpp`. Java and Objective-C class
files follow the language-standard class naming (for example
`NativeBridge.java` and `ViewController.mm`); platform-mandated names such
as `CMakeLists.txt`, `AndroidManifest.xml`, and `Info.plist` remain unchanged.

## Build the Android sample

~~~bash
./gradlew :samples:android:app:assembleDebug
~~~

The APK is produced at
samples/android/app/build/outputs/apk/debug/app-debug.apk.

## Embed in another Android project

Compile the sources listed by the root CMakeLists.txt, add core/include,
third_party/spdlog/include, and third_party/glm to the include path, then link
android, log, and vulkan. Reuse platform/android as the thin JNI shell.

## Other platforms

Implement evk::IPlatform for the target window system. Platform code only owns
the Vulkan surface, VSync source, pointer translation, and surface lifecycle;
the Widget, Element, State, navigation, and render trees stay shared.
