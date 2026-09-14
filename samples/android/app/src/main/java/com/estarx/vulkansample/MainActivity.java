package com.estarx.vulkansample;

import android.os.Build;
import android.os.Bundle;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;

import com.estarx.vulkan.NativeBridge;

public class MainActivity extends AppCompatActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 边到边布局：内容延伸到状态栏/手势条之下（配合主题的 windowFullscreen），
        // 遮挡区域由 SafeAreaChanged 事件通知 App 内缩，而不是系统裁剪窗口。
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        setContentView(R.layout.activity_main);
        // 平台通道演示：引擎 invokePlatform("deviceInfo") 的出向落点，
        // 返回"厂商 型号"；未匹配的方法返回空串（= 未实现）。
        NativeBridge.setPlatformHandler((method, args) ->
            "deviceInfo".equals(method) ? Build.MANUFACTURER + " " + Build.MODEL : "");
    }

    // 系统返回（返回键或手势导航侧滑）统一走 native：导航栈能 pop 就消费，
    // 到栈底则交还系统默认行为（finish Activity）。
    @Override
    public void onBackPressed() {
        if (!NativeBridge.nativeOnBackPressed()) {
            super.onBackPressed();
        }
    }
}
