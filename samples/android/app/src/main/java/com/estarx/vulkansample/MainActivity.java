package com.estarx.vulkansample;

import android.os.Bundle;
import androidx.appcompat.app.AppCompatActivity;

import com.estarx.vulkan.NativeBridge;

public class MainActivity extends AppCompatActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
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
