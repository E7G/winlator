package com.winlator.cmod.xenvironment.components;

import android.os.Build;
import android.util.Log;

import com.winlator.cmod.renderer.AHardwareBufferPool;
import com.winlator.cmod.renderer.VulkanRenderer;
import com.winlator.cmod.xenvironment.EnvironmentComponent;

/**
 * Owns the AHB pool used by the 3.1 DirectAHBCompositor.
 * The normal Vulkan renderer stays alive as an automatic fallback.
 */
public class DirectCompositorComponent extends EnvironmentComponent {
    private static final String TAG = "DirectAHB";
    private final VulkanRenderer renderer;
    private final int poolSize;
    private final int width;
    private final int height;
    private final float refreshRate;
    private AHardwareBufferPool pool;
    private volatile boolean ready;

    public DirectCompositorComponent(VulkanRenderer renderer, int poolSize,
                                     int width, int height, float refreshRate) {
        this.renderer = renderer;
        this.poolSize = Math.max(3, Math.min(4, poolSize));
        this.width = width;
        this.height = height;
        this.refreshRate = refreshRate;
    }

    @Override
    public void start() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Log.w(TAG, "DAC needs Android 10/API 29+; Vulkan fallback remains active");
            return;
        }
        pool = new AHardwareBufferPool(width, height, poolSize);
        ready = pool.init();
        if (!ready) {
            pool.destroy();
            pool = null;
            Log.w(TAG, "AHB pool unavailable; Vulkan fallback remains active");
        } else {
            Log.i(TAG, "AHB pool ready: " + poolSize + " x " + width + "x" + height);
        }
    }

    @Override
    public void stop() {
        ready = false;
        renderer.stopDirectAHBReceiver();
        if (pool != null) {
            pool.destroy();
            pool = null;
        }
    }

    public boolean isReady() { return ready && pool != null; }
    public AHardwareBufferPool getPool() { return pool; }
    public VulkanRenderer getRenderer() { return renderer; }
    public float getRefreshRate() { return refreshRate; }
}
