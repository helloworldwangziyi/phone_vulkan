import { VulkanShell, nativeBridge } from '../vulkan/VulkanShell';

@Entry
@Component
struct Index {
  build() {
    Column() {
      VulkanShell()
    }
    .width('100%')
    .height('100%')
  }

  // 系统返回（手势导航侧滑/返回键）统一走 native：导航栈能 pop 就消费，
  // 到栈底则返回 false 交还系统默认行为（对应 Android MainActivity.onBackPressed）。
  // 注意：onBackPress 只对 @Entry 页面组件生效，所以放在这里而不是壳组件里。
  onBackPress(): boolean {
    return nativeBridge()?.onBackPressed() ?? false;
  }
}
