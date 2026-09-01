import displaySync from '@ohos.graphics.displaySync';
import { window } from '@kit.ArkUI';

/**
 * native 模块句柄：函数由 platform/hongmeng/cpp/hongmeng_napi_bridge.cpp 导出。
 * 句柄经 XComponent 的 onLoad(context) 获得（context 即 NAPI 模块的 exports，
 * 等价于 import 'libestarx_vulkan.so' 的返回值；刻意不手动 import——手动 import
 * 会让模块先于 XComponent 加载，Init 解包不到 XComponent 实例对象）。
 */
interface NativeBridge {
  setStoragePath(path: string): void;
  beginFrame(frameTimeNanos: number): void;
  onBackPressed(): boolean;
  safeAreaChanged(top: number, bottom: number, left: number, right: number): void;
}

// 页面级的 onBackPress 只能用 @Entry 组件实现（见 pages/Index.ets），
// 所以 native 句柄提到模块级共享，壳组件加载完成前为 null。
let g_bridge: NativeBridge | null = null;

/** 取 native 桥句柄；XComponent onLoad 之前返回 null（调用方自行判空）。 */
export function nativeBridge(): NativeBridge | null {
  return g_bridge;
}

/**
 * 鸿蒙薄壳视图：XComponent(SURFACE) + DisplaySync，只负责把 surface、
 * 帧节奏、安全区交给 native，自身不含任何业务逻辑（"薄壳"的含义）。
 * 对应 Android 的 VulkanSurfaceView（Choreographer + WindowInsets）。
 *
 * 线程模型：XComponent 回调、DisplaySync 帧回调、本文件的 NAPI 调用全部在
 * UI 线程，与 Android/iOS 壳一致，core 的无锁单线程假设原样成立。
 */
@Component
export struct VulkanShell {
  // 注意：字段不能叫 displaySync——会遮蔽上面导入的同名模块命名空间。
  private frameSync: displaySync.DisplaySync | null = null;
  private mainWindow: window.Window | null = null;

  aboutToAppear(): void {
    // 安全区监听：主题是边到边全屏，视图延伸到状态栏/手势条之下，
    // 必须把系统占用区（含刘海 cutout）通知 App 做布局内缩。
    // AvoidArea 矩形单位是像素，与 surface 坐标系一致。
    window.getLastWindow(getContext(this)).then((win: window.Window) => {
      this.mainWindow = win;
      win.on('avoidAreaChange', () => this.reportSafeArea());
      this.reportSafeArea();
    }).catch(() => {
      // 主窗口尚未就绪（理论上 loadContent 后不会走到），放弃本次监听。
    });
  }

  aboutToDisappear(): void {
    this.stopFrames();
    this.mainWindow?.off('avoidAreaChange');
    this.mainWindow = null;
  }

  build() {
    XComponent({ id: 'estarx', type: XComponentType.SURFACE, libraryname: 'estarx_vulkan' })
      .width('100%')
      .height('100%')
      .onLoad((context?: object) => {
        if (!context) {
          return;
        }
        // object → NativeBridge 是向下转型（NativeBridge 可赋给 object），
        // 合法；ArkTS 禁 any/unknown，不能用 as unknown 中转。
        g_bridge = context as NativeBridge;
        // 先于引擎就绪注入私有存储目录：core 的 KeyValueStore 初始化需要平台路径。
        // onLoad 官方保证先于 native 侧 OnSurfaceCreated 触发，时序安全。
        g_bridge.setStoragePath(getContext(this).filesDir);
        // native 句柄就绪：补报一次当前安全区，并启动帧节奏。
        this.reportSafeArea();
        this.startFrames();
      })
      .onDestroy(() => {
        this.stopFrames();
        g_bridge = null;
      })
  }

  // DisplaySync 是 ArkTS 侧的 VSync 通道（对应 Android 的 Choreographer），
  // 回调在 UI 线程；native 侧仅在 dirty 时真正构建和提交帧。
  private startFrames(): void {
    if (this.frameSync) {
      return;
    }
    const sync = displaySync.create();
    sync.on('frame', (intervalInfo: displaySync.IntervalInfo) => {
      g_bridge?.beginFrame(intervalInfo.timestamp);
    });
    sync.start();
    this.frameSync = sync;
  }

  private stopFrames(): void {
    this.frameSync?.stop();
    this.frameSync = null;
  }

  // 合并三类系统占用区（状态栏 TYPE_SYSTEM / 刘海 TYPE_CUTOUT /
  // 手势条 TYPE_NAVIGATION_INDICATOR），取四边最大内边距上报 native。
  private reportSafeArea(): void {
    const win = this.mainWindow;
    if (!win || !g_bridge) {
      return;
    }
    let top = 0;
    let bottom = 0;
    let left = 0;
    let right = 0;
    const types: window.AvoidAreaType[] = [
      window.AvoidAreaType.TYPE_SYSTEM,
      window.AvoidAreaType.TYPE_CUTOUT,
      window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR,
    ];
    for (const type of types) {
      try {
        const area = win.getWindowAvoidArea(type);
        top = Math.max(top, area.topRect.height);
        bottom = Math.max(bottom, area.bottomRect.height);
        left = Math.max(left, area.leftRect.width);
        right = Math.max(right, area.rightRect.width);
      } catch (e) {
        // 区域类型在某些形态（如 2in1）下可能查询失败，忽略该类型。
      }
    }
    g_bridge.safeAreaChanged(top, bottom, left, right);
  }
}
