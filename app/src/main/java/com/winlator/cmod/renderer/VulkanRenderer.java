package com.winlator.cmod.renderer;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.view.Surface;
import android.widget.Toast;

import com.winlator.cmod.R;
import com.winlator.cmod.widget.FrameRating;
import com.winlator.cmod.widget.WinlatorHUD;
import com.winlator.cmod.widget.XServerView;
import com.winlator.cmod.xserver.Bitmask;
import com.winlator.cmod.xserver.Cursor;
import com.winlator.cmod.xserver.Drawable;
import com.winlator.cmod.xserver.Pointer;
import com.winlator.cmod.xserver.Window;
import com.winlator.cmod.xserver.WindowAttributes;
import com.winlator.cmod.xserver.WindowManager;
import com.winlator.cmod.xserver.XLock;
import com.winlator.cmod.xserver.XServer;

import java.util.ArrayList;

import dalvik.annotation.optimization.FastNative;

public class VulkanRenderer implements WindowManager.OnWindowModificationListener,
                                       Pointer.OnPointerMotionListener {

    static { System.loadLibrary("vulkan_renderer"); }

    public final XServerView xServerView;
    private final XServer xServer;
    private long nativeHandle = 0;
    private final Object lock = new Object();

    public final ViewTransformation viewTransformation = new ViewTransformation();
    private boolean fullscreen = false;
    private float magnifierZoom = 1.0f;
    private boolean screenOffsetYRelativeToCursor = false;
    public int surfaceWidth;
    public int surfaceHeight;
    private String[] unviewableWMClasses = null;
    private boolean cursorVisible = false;
    private String driverPath = null;
    private java.util.concurrent.ExecutorService initExecutor = null;
    private volatile boolean initComplete = false;
    private volatile boolean inPipMode = false;
    private String driverLibraryName = null;
    private String nativeLibDir = null;
    private Drawable rootCursorDrawable;
    private Cursor lastCursor = null;
    private volatile boolean directAHBEnabled = false;
    private volatile boolean directAHBActive = false;
    private volatile int directAHBWindowId = -1;
    private float directAHBRefreshRate = 0.0f;
    private long directAHBFrameCount = 0;

    private volatile ArrayList<RenderableWindow> renderableWindows = new ArrayList<>();

    public VulkanRenderer(XServerView xServerView, XServer xServer) {
        this.xServerView = xServerView;
        this.xServer = xServer;
        rootCursorDrawable = createRootCursorDrawable();
        xServer.windowManager.addOnWindowModificationListener(this);
        xServer.pointer.addOnPointerMotionListener(this);
    }

    private Drawable createRootCursorDrawable() {
        try {
            Context context = xServerView.getContext();
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inScaled = false;
            Bitmap bitmap = BitmapFactory.decodeResource(context.getResources(), R.drawable.cursor, options);
            return Drawable.fromBitmap(bitmap);
        } catch (Exception e) { return null; }
    }

    private native long nativeInit(Surface surface, int screenWidth, int screenHeight,
        String driverPath, String libraryName, String nativeLibDir);
    private native void nativeResize(long handle, int width, int height);
    private native void nativeDestroy(long handle);
    private native void nativeDetachSurface(long handle);
    private native boolean nativeReattachSurface(long handle, Surface surface);
    private native int[] nativeGetSwapchainSize(long handle);
    private native boolean nativeStartDirectAHB(long handle, int socketFd,
        long b0, long b1, long b2, long b3, int count,
        int logicalWidth, int logicalHeight, float refreshRate);
    private native void nativeStopDirectAHB(long handle);
    private native boolean nativeIsDirectAHBPresenting(long handle);
    private native boolean nativeSubmitDirectAHB(long handle, long ahbPtr, int acquireFenceFd,
        int srcWidth, int srcHeight, int dstX, int dstY, int dstWidth, int dstHeight,
        float refreshRate);
    private native void nativeHideDirectAHB(long handle);

    @FastNative private native void nativeUpdateWindowContent(long handle, long id,
        java.nio.ByteBuffer pixels, short width, short height, short stride, int x, int y);
    @FastNative private native void nativeUpdateWindowContentAHB(long handle, long id,
        long ahbPtr, short width, short height, int x, int y);
    @FastNative private native void nativeSetTransformAndScissor(long handle,
        float ox, float oy, float sx, float sy,
        boolean hasScissor, int scX, int scY, int scW, int scH);
    @FastNative private native void nativeSetPointerPos(long handle, short x, short y);
    @FastNative private native void nativeSetCursorVisible(long handle, boolean visible);
    @FastNative private native void nativeUpdateCursorImage(long handle,
        java.nio.ByteBuffer pixels, short width, short height, short hotX, short hotY);
    @FastNative private native void nativeSetRenderList(long handle,
        long[] ids, int[] xs, int[] ys, int count);
    @FastNative private native void nativeRemoveWindow(long handle, long id);
    @FastNative private native void nativeSetVerboseLog(long handle, boolean v);
    @FastNative private native void nativeSetSharpness(long handle, float sharpness);

    private native void nativeDumpRendererInfo(long handle);
    private native void nativeSetFilterMode(long handle, int mode);
    private native void nativeSetStretchMode(long handle, int mode);
    private native void nativeSetPostFXMode(long handle, int mode);
    private native void nativeSetSwapRB(long handle, boolean enabled);
    private native void nativeSetPresentMode(long handle, int mode);
    private native int[] nativeGetSupportedPresentModes(long handle);


    private static volatile boolean gpuImageChecked = false;

    private static long did(Drawable d) {
        return (long) System.identityHashCode(d);
    }


    public void onSurfaceCreated(Surface surface) {
        if (!gpuImageChecked) { GPUImage.checkIsSupported(); gpuImageChecked = true; }
        if (initExecutor != null) {
            initExecutor.shutdownNow();
            try { initExecutor.awaitTermination(3, java.util.concurrent.TimeUnit.SECONDS); }
            catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        }
        initExecutor = java.util.concurrent.Executors.newSingleThreadExecutor();
        initExecutor.execute(() -> {
            synchronized (lock) {
                if (nativeHandle != 0) {
                    boolean ok = nativeReattachSurface(nativeHandle, surface);
                    if (!ok) {
                        nativeDestroy(nativeHandle);
                        nativeHandle = 0;
                    } else {
                        int[] sc = nativeGetSwapchainSize(nativeHandle);
                        if (sc != null && sc[0] > 0 && sc[1] > 0) {
                            surfaceWidth = sc[0]; surfaceHeight = sc[1];
                            viewTransformation.update(surfaceWidth, surfaceHeight,
                                xServer.screenInfo.width, xServer.screenInfo.height);
                        }
                        nativeSetPresentMode(nativeHandle, pendingPresentMode);
                        nativeSetFilterMode(nativeHandle, pendingFilterMode);
                        nativeSetSwapRB(nativeHandle, pendingSwapRB);
                        nativeSetStretchMode(nativeHandle, pendingStretchMode);
                        nativeSetPostFXMode(nativeHandle, pendingPostFXMode);
                        nativeSetSharpness(nativeHandle, pendingSharpness);
                        updateTransform();
                        nativeSetCursorVisible(nativeHandle, cursorVisible);
                        initComplete = true;
                        xServerView.queueEvent(this::updateScene);
                        return;
                    }
                }
                nativeHandle = nativeInit(surface,
                    xServer.screenInfo.width, xServer.screenInfo.height,
                    driverPath, driverLibraryName, nativeLibDir);
                if (nativeHandle != 0) {
                    nativeSetPresentMode(nativeHandle, pendingPresentMode);
                    nativeSetFilterMode(nativeHandle, pendingFilterMode);
                    nativeSetSwapRB(nativeHandle, pendingSwapRB);
                    nativeSetPostFXMode(nativeHandle, pendingPostFXMode);
                    nativeSetSharpness(nativeHandle, pendingSharpness);
                    updateTransform();
                    nativeSetCursorVisible(nativeHandle, cursorVisible);
                }
            }
            synchronized (lock) {
                if (nativeHandle != 0) {
                    nativeSetVerboseLog(nativeHandle, true);
                    nativeDumpRendererInfo(nativeHandle);
                }
            }
            initComplete = true;
            xServerView.queueEvent(this::updateScene);
        });
    }

    public void onSurfaceChanged(int width, int height) {
        if (inPipMode) return;
        surfaceWidth = width; surfaceHeight = height;
        viewTransformation.update(width, height, xServer.screenInfo.width, xServer.screenInfo.height);
        synchronized (lock) {
            if (nativeHandle != 0) { nativeResize(nativeHandle, width, height); updateTransform(); }
        }
    }

    public void onSurfaceDestroyed() {
        initComplete = false;
        if (initExecutor != null) {
            initExecutor.shutdownNow();
            try { initExecutor.awaitTermination(3, java.util.concurrent.TimeUnit.SECONDS); }
            catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
            initExecutor = null;
        }
        synchronized (lock) {
            if (nativeHandle != 0) nativeDetachSurface(nativeHandle);
        }
    }

    public void forceCleanup() {
        initComplete = false;
        if (initExecutor != null) {
            initExecutor.shutdownNow();
            try { initExecutor.awaitTermination(3, java.util.concurrent.TimeUnit.SECONDS); }
            catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
            initExecutor = null;
        }
        synchronized (lock) {
            if (nativeHandle != 0) {
                nativeDestroy(nativeHandle);
                nativeHandle = 0;
            }
        }
    }

    public boolean startDirectAHBReceiver(int socketFd, long b0, long b1, long b2, long b3,
                                          int count, int logicalWidth, int logicalHeight,
                                          float refreshRate) {
        synchronized (lock) {
            return nativeHandle != 0 && nativeStartDirectAHB(nativeHandle, socketFd,
                    b0, b1, b2, b3, count, logicalWidth, logicalHeight, refreshRate);
        }
    }

    public void stopDirectAHBReceiver() {
        synchronized (lock) {
            if (nativeHandle != 0) nativeStopDirectAHB(nativeHandle);
        }
    }

    public boolean isDirectAHBPresenting() {
        synchronized (lock) {
            return nativeHandle != 0 && nativeIsDirectAHBPresenting(nativeHandle);
        }
    }


    public void setDirectAHBEnabled(boolean enabled) {
        directAHBEnabled = enabled;
        if (!enabled) hideDirectAHB();
    }

    public boolean isDirectAHBEnabled() { return directAHBEnabled; }
    public boolean isDirectAHBActive() { return directAHBActive; }
    public long getDirectAHBFrameCount() { return directAHBFrameCount; }

    private float getDirectAHBRefreshRate() {
        if (directAHBRefreshRate > 1.0f) return directAHBRefreshRate;
        float best = 60.0f;
        try {
            android.view.Display display = xServerView.getDisplay();
            if (display != null) {
                for (android.view.Display.Mode mode : display.getSupportedModes())
                    best = Math.max(best, mode.getRefreshRate());
            }
        } catch (Throwable ignored) {}
        directAHBRefreshRate = best;
        return best;
    }

    private void hideDirectAHB() {
        synchronized (lock) {
            if (nativeHandle != 0 && directAHBActive)
                nativeHideDirectAHB(nativeHandle);
            directAHBActive = false;
            directAHBWindowId = -1;
        }
    }

    private boolean trySubmitDirectAHB(Window window, Drawable pixmap, short xOff, short yOff) {
        if (!directAHBEnabled || android.os.Build.VERSION.SDK_INT < 29 ||
                nativeHandle == 0 || pixmap == null || !pixmap.isDirectScanout() ||
                !(pixmap.getTexture() instanceof GPUImage) ||
                !window.attributes.isMapped() || inPipMode ||
                magnifierZoom != 1.0f || screenOffsetYRelativeToCursor ||
                pendingFilterMode != 0 || pendingStretchMode != 0 ||
                pendingPostFXMode != 0 || pendingSwapRB ||
                xOff != 0 || yOff != 0) {
            if (directAHBActive) hideDirectAHB();
            return false;
        }

        final int screenW = xServer.screenInfo.width;
        final int screenH = xServer.screenInfo.height;
        if (pixmap.width < screenW || pixmap.height < screenH ||
                window.getRootX() != 0 || window.getRootY() != 0) {
            if (directAHBActive) hideDirectAHB();
            return false;
        }

        GPUImage image = (GPUImage) pixmap.getTexture();
        long ahbPtr = image.getHardwareBufferPtr();
        if (ahbPtr == 0) {
            if (directAHBActive) hideDirectAHB();
            return false;
        }

        final int dx, dy, dw, dh;
        if (fullscreen) {
            dx = 0; dy = 0;
            dw = Math.max(1, surfaceWidth);
            dh = Math.max(1, surfaceHeight);
        } else {
            viewTransformation.update(surfaceWidth, surfaceHeight, screenW, screenH);
            dx = viewTransformation.viewOffsetX;
            dy = viewTransformation.viewOffsetY;
            dw = Math.max(1, viewTransformation.viewWidth);
            dh = Math.max(1, viewTransformation.viewHeight);
        }

        boolean ok = nativeSubmitDirectAHB(nativeHandle, ahbPtr, -1,
                pixmap.width, pixmap.height, dx, dy, dw, dh,
                getDirectAHBRefreshRate());
        if (ok) {
            directAHBActive = true;
            directAHBWindowId = window.id;
            directAHBFrameCount++;
            return true;
        }

        directAHBActive = false;
        directAHBWindowId = -1;
        return false;
    }


    private void updateTransform() {
        if (nativeHandle == 0) return;
        final float zoom = magnifierZoom;
        final float ptrX = xServer.pointer.getX();
        final float ptrY = xServer.pointer.getY();

        final float ox, oy, sx, sy;
        if (fullscreen) {
            viewTransformation.update(surfaceWidth, surfaceHeight,
                xServer.screenInfo.width, xServer.screenInfo.height);
            if (zoom != 1.0f) {
                ox = viewTransformation.sceneOffsetX + ptrX * viewTransformation.sceneScaleX * (1f - zoom);
                oy = viewTransformation.sceneOffsetY + ptrY * viewTransformation.sceneScaleY * (1f - zoom);
                sx = viewTransformation.sceneScaleX * zoom;
                sy = viewTransformation.sceneScaleY * zoom;
            } else {
                ox = 0f; oy = 0f; sx = 1f; sy = 1f;
            }
        } else {
            float py = 0;
            if (screenOffsetYRelativeToCursor) {
                short halfH = (short)(xServer.screenInfo.height / 2);
                py = Math.max(0, Math.min(ptrY - halfH / 2.0f, halfH));
            }
            ox = viewTransformation.sceneOffsetX + ptrX * viewTransformation.sceneScaleX * (1f - zoom);
            oy = viewTransformation.sceneOffsetY - py + ptrY * viewTransformation.sceneScaleY * (1f - zoom);
            sx = viewTransformation.sceneScaleX * zoom;
            sy = viewTransformation.sceneScaleY * zoom;
        }

        final boolean needScissor = !(fullscreen && zoom == 1.0f);
        nativeSetTransformAndScissor(nativeHandle, ox, oy, sx, sy,
            needScissor,
            viewTransformation.viewOffsetX, viewTransformation.viewOffsetY,
            viewTransformation.viewWidth,   viewTransformation.viewHeight);
    }

    public void updateScene() {
        ArrayList<RenderableWindow> newList = new ArrayList<>();
        try (XLock xl = xServer.lock(XServer.Lockable.WINDOW_MANAGER, XServer.Lockable.DRAWABLE_MANAGER)) {
            collectWindows(newList, xServer.windowManager.rootWindow,
                xServer.windowManager.rootWindow.getX(),
                xServer.windowManager.rootWindow.getY());
        }
        synchronized (lock) {
            renderableWindows = newList;
            pushRenderList(newList);
        }
    }

    private void collectWindows(ArrayList<RenderableWindow> list, Window window, int x, int y) {
        if (!window.attributes.isMapped()) return;
        if (window != xServer.windowManager.rootWindow) {
            boolean viewable = true;
            if (unviewableWMClasses != null) {
                String wc = window.getClassName();
                for (String cls : unviewableWMClasses) {
                    if (wc.contains(cls)) {
                        if (window.attributes.isEnabled()) window.disableAllDescendants();
                        viewable = false; break;
                    }
                }
            }
            if (viewable) list.add(new RenderableWindow(window.getContent(), x, y));
        }
        for (Window child : window.getChildren())
            collectWindows(list, child, child.getX() + x, child.getY() + y);
    }

    private void pushRenderList(ArrayList<RenderableWindow> list) {
        if (nativeHandle == 0) return;
        final int screenW = xServer.screenInfo.width, screenH = xServer.screenInfo.height;
        int start = 0;
        for (int i = list.size() - 1; i >= 0; i--) {
            RenderableWindow rw = list.get(i);
            if (rw.content != null && rw.content.width >= screenW && rw.content.height >= screenH) {
                start = i; break;
            }
        }
        final int n = list.size() - start;
        if (n <= 0) { nativeSetRenderList(nativeHandle, new long[0], new int[0], new int[0], 0); return; }
        long[] ids = new long[n]; int[] xs = new int[n]; int[] ys = new int[n];
        for (int i = 0; i < n; i++) {
            RenderableWindow rw = list.get(start + i);
            ids[i] = did(rw.content); xs[i] = rw.rootX; ys[i] = rw.rootY;
        }
        nativeSetRenderList(nativeHandle, ids, xs, ys, n);
    }


    private void sendCursorToNative(Cursor cursor) {
        if (nativeHandle == 0) return;
        Drawable cd; short hotX = 0, hotY = 0;
        boolean effVis = cursorVisible;
        if (cursor != null) {
            if (!cursor.isVisible()) effVis = false;
            cd = cursor.cursorImage; hotX = (short)cursor.hotSpotX; hotY = (short)cursor.hotSpotY;
        } else { cd = rootCursorDrawable; }
        nativeSetCursorVisible(nativeHandle, effVis);
        if (effVis && cd != null && cd.getBuffer() != null) {
            synchronized (cd.renderLock) {
                nativeUpdateCursorImage(nativeHandle, cd.getBuffer(), cd.width, cd.height, hotX, hotY);
            }
        }
    }

    public void onUpdateWindowContentDirect(Window window, Drawable pixmap, short xOff, short yOff) {
        if (window.id == fpsWindowId) {
            if (hudRef != null) hudRef.onFrame();
            if (classicHudRef != null) classicHudRef.update();
        }
        synchronized (lock) {
            if (trySubmitDirectAHB(window, pixmap, xOff, yOff)) return;
            if (nativeHandle == 0 || pixmap == null) return;
            final int rx = window.getRootX() + xOff, ry = window.getRootY() + yOff;
            synchronized (pixmap.renderLock) {
                if (pixmap.getTexture() instanceof GPUImage) {
                    GPUImage g = (GPUImage) pixmap.getTexture();
                    final long ahbPtr = g.getHardwareBufferPtr();
                    if (ahbPtr != 0) {
                        nativeUpdateWindowContentAHB(nativeHandle, did(window.getContent()),
                            ahbPtr, pixmap.width, pixmap.height, rx, ry);
                        return;
                    }
                    java.nio.ByteBuffer vd = g.getVirtualData();
                    if (vd != null) {
                        short s = g.getStride() > 0 ? g.getStride() : pixmap.width;
                        nativeUpdateWindowContent(nativeHandle, did(window.getContent()),
                            vd, pixmap.width, pixmap.height, s, rx, ry);
                        return;
                    }
                }
                java.nio.ByteBuffer buf = pixmap.getBuffer();
                if (buf == null) return;
                short stride = (short)(buf.capacity() / (pixmap.height * 4));
                nativeUpdateWindowContent(nativeHandle, did(window.getContent()),
                    buf, pixmap.width, pixmap.height, stride, rx, ry);
            }
        }
    }

    @Override
    public void onUpdateWindowContent(Window window) {
        synchronized (lock) {
            if (nativeHandle == 0) return;
            if (directAHBActive) hideDirectAHB();
            Drawable drawable = window.getContent();
            if (drawable == null || !window.attributes.isMapped()) return;
            if (unviewableWMClasses != null) {
                String wc = window.getClassName();
                for (String cls : unviewableWMClasses) if (wc.contains(cls)) return;
            }
            final int rx = window.getRootX(), ry = window.getRootY();
            synchronized (drawable.renderLock) {
                if (drawable.getTexture() instanceof GPUImage) {
                    GPUImage g = (GPUImage) drawable.getTexture();
                    final long ahbPtr = g.getHardwareBufferPtr();
                    if (ahbPtr != 0) {
                        nativeUpdateWindowContentAHB(nativeHandle, did(drawable),
                            ahbPtr, drawable.width, drawable.height, rx, ry);
                        return;
                    }
                    java.nio.ByteBuffer vd = g.getVirtualData();
                    if (vd != null) {
                        short s = g.getStride() > 0 ? g.getStride() : drawable.width;
                        nativeUpdateWindowContent(nativeHandle, did(drawable),
                            vd, drawable.width, drawable.height, s, rx, ry);
                        return;
                    }
                }
                java.nio.ByteBuffer buf = drawable.getBuffer();
                if (buf == null) return;
                short stride = (short)(buf.capacity() / (drawable.height * 4));
                nativeUpdateWindowContent(nativeHandle, did(drawable),
                    buf, drawable.width, drawable.height, stride, rx, ry);
            }
        }
    }

    @Override
    public void onPointerMove(short x, short y) {
        synchronized (lock) {
            if (nativeHandle == 0) return;
            nativeSetPointerPos(nativeHandle, x, y);
            Window pw = xServer.inputDeviceManager.getPointWindow();
            Cursor cursor = pw != null ? pw.attributes.getCursor() : null;
            if (cursor != lastCursor) { lastCursor = cursor; sendCursorToNative(cursor); }
            if (screenOffsetYRelativeToCursor || magnifierZoom != 1.0f) updateTransform();
        }
    }


    @Override
    public void onDestroyWindow(Window window) {
        if (directAHBActive && window.id == directAHBWindowId) hideDirectAHB();
        final long id = did(window.getContent());
        xServerView.queueEvent(() -> {
            synchronized (lock) {
                if (nativeHandle != 0) nativeRemoveWindow(nativeHandle, id);
            }
            updateScene();
        });
    }

    @Override public void onMapWindow(Window window) { xServerView.queueEvent(this::updateScene); }

    @Override
    public void onUnmapWindow(Window window) {
        if (directAHBActive && window.id == directAHBWindowId) hideDirectAHB();
        final long id = did(window.getContent());
        xServerView.queueEvent(() -> {
            synchronized (lock) {
                if (nativeHandle != 0) nativeRemoveWindow(nativeHandle, id);
            }
            updateScene();
        });
    }

    @Override public void onChangeWindowZOrder(Window window) { xServerView.queueEvent(this::updateScene); }

    @Override
    public void onUpdateWindowGeometry(Window window, boolean resized) {
        xServerView.queueEvent(this::updateScene);
    }

    @Override
    public void onUpdateWindowAttributes(Window window, Bitmask mask) {
        if (mask.isSet(WindowAttributes.FLAG_CURSOR)) {
            synchronized (lock) {
                Window pw = xServer.inputDeviceManager.getPointWindow();
                if (pw == window) { lastCursor = window.attributes.getCursor(); sendCursorToNative(lastCursor); }
            }
        }
    }

    public void setCursorVisible(boolean visible) {
        cursorVisible = visible;
        synchronized (lock) {
            if (nativeHandle != 0) { nativeSetCursorVisible(nativeHandle, visible); if (visible) sendCursorToNative(lastCursor); }
        }
    }

    public boolean isCursorVisible() { return cursorVisible; }

    public void setDriverInfo(String driverPath, String libraryName, String nativeLibDir) {
        this.driverPath = driverPath;
        this.driverLibraryName = libraryName;
        this.nativeLibDir = nativeLibDir;
        android.util.Log.d("Winlator_Renderer",
            "setDriverInfo: path=" + driverPath + " lib=" + libraryName);
    }

    public void setVerboseLog(boolean v) {
        synchronized (lock) { if (nativeHandle != 0) nativeSetVerboseLog(nativeHandle, v); }
    }

    public void dumpRendererInfo() {
        synchronized (lock) { if (nativeHandle != 0) nativeDumpRendererInfo(nativeHandle); }
    }

    public void setStretchMode(int mode) {
        pendingStretchMode = mode;
        if (mode != 0 && directAHBActive) hideDirectAHB();
        synchronized (lock) { if (nativeHandle != 0) nativeSetStretchMode(nativeHandle, mode); }
    }

    public void setPostFXMode(int mode) {
        pendingPostFXMode = mode;
        if (mode != 0 && directAHBActive) hideDirectAHB();
        synchronized (lock) { if (nativeHandle != 0) nativeSetPostFXMode(nativeHandle, mode); }
    }

    public void setSharpness(float s) {
        pendingSharpness = s;
        synchronized (lock) { if (nativeHandle != 0) nativeSetSharpness(nativeHandle, s); }
    }

    public void setFilterMode(int mode) {
        pendingFilterMode = mode;
        if (mode != 0 && directAHBActive) hideDirectAHB();
        synchronized (lock) { if (nativeHandle != 0) nativeSetFilterMode(nativeHandle, mode); }
    }

    public void setSwapRB(boolean enabled) {
        pendingSwapRB = enabled;
        if (enabled && directAHBActive) hideDirectAHB();
        synchronized (lock) { if (nativeHandle != 0) nativeSetSwapRB(nativeHandle, enabled); }
    }

    public void setVkPresentMode(int mode) {
        pendingPresentMode = mode;
        synchronized (lock) { if (nativeHandle != 0) nativeSetPresentMode(nativeHandle, mode); }
    }

    public int[] getSupportedPresentModes() {
        synchronized (lock) {
            if (nativeHandle != 0) return nativeGetSupportedPresentModes(nativeHandle);
        }
        return new int[0];
    }

    public boolean isFullscreen() { return fullscreen; }
    public void toggleFullscreen() {
        fullscreen = !fullscreen;
        synchronized (lock) { updateTransform(); }
        xServerView.queueEvent(this::updateScene);
    }
    public void setScreenOffsetYRelativeToCursor(boolean b) {
        screenOffsetYRelativeToCursor = b;
        if (b && directAHBActive) hideDirectAHB();
        synchronized (lock) { updateTransform(); }
    }
    public boolean isScreenOffsetYRelativeToCursor() { return screenOffsetYRelativeToCursor; }
    public void setMagnifierZoom(float zoom) {
        magnifierZoom = zoom;
        if (zoom != 1.0f && directAHBActive) hideDirectAHB();
        synchronized (lock) { updateTransform(); }
    }
    public float getMagnifierZoom() { return magnifierZoom; }
    public void setUnviewableWMClasses(String... classes) { this.unviewableWMClasses = classes; }

    private int     pendingPresentMode    = 2;
    private int     pendingStretchMode    = 0;
    private int     pendingFilterMode     = 0;
    private int     pendingPostFXMode     = 0;
    private float   pendingSharpness      = 0.5f;
    private boolean pendingSwapRB         = false;

    private WinlatorHUD hudRef = null;
    private FrameRating classicHudRef = null;
    private int fpsWindowId = -1;
    private int fpsLimit = 0;

    public void setFpsWindowId(int id) { fpsWindowId = id; }
    public void setFrameRating(Object fr) {
        if (fr instanceof WinlatorHUD) hudRef = (WinlatorHUD) fr;
        else if (fr instanceof FrameRating) classicHudRef = (FrameRating) fr;
    }
    public int getFpsLimit() { return fpsLimit; }
    public void setFpsLimit(int limit) { this.fpsLimit = limit; }
    public void setPipMode(boolean pip) {
        inPipMode = pip;
        if (pip && directAHBActive) hideDirectAHB();
    }
    public int getSurfaceWidth() { return surfaceWidth; }
    public int getSurfaceHeight() { return surfaceHeight; }
    public void requestRender() {}


    private static class RenderableWindow {
        public final Drawable content; public int rootX, rootY;
        public RenderableWindow(Drawable c, int x, int y) { content=c; rootX=x; rootY=y; }
    }
}
