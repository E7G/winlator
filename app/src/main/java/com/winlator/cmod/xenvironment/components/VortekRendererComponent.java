package com.winlator.cmod.xenvironment.components;

import android.content.Context;

import androidx.annotation.Keep;

import com.winlator.cmod.xconnector.Client;
import com.winlator.cmod.xconnector.ConnectionHandler;
import com.winlator.cmod.xconnector.RequestHandler;
import com.winlator.cmod.xconnector.UnixSocketConfig;
import com.winlator.cmod.xconnector.XConnectorEpoll;
import com.winlator.cmod.xconnector.XInputStream;
import com.winlator.cmod.xenvironment.EnvironmentComponent;
import com.winlator.cmod.xserver.Drawable;
import com.winlator.cmod.xserver.Window;
import com.winlator.cmod.xserver.XServer;

import java.io.IOException;

/**
 * Host side of Vortek's Vulkan transport.
 *
 * The guest ICD (vortek-2.1.tzst) sends Vulkan requests over an AF_UNIX socket;
 * this component owns the Android Vulkan context and exposes the X11 window's
 * AHardwareBuffer to Vortek for presentation.
 */
public class VortekRendererComponent extends EnvironmentComponent implements ConnectionHandler, RequestHandler {
    private static final byte REQUEST_CODE_CREATE_CONTEXT = 1;
    private static final byte REQUEST_CODE_SEND_EXTRA_DATA = 2;

    public static final short IMAGE_CACHE_SIZE = 256;
    // VK_MAKE_API_VERSION(0, 1, 3, 128)
    public static final int VK_MAX_VERSION = (1 << 22) | (3 << 12) | 128;

    private final Context context;
    private final XServer xServer;
    private final UnixSocketConfig socketConfig;
    private final Options options;
    private XConnectorEpoll connector;

    static {
        System.loadLibrary("vortekrenderer");
    }

    public static class Options {
        public int vkMaxVersion = VK_MAX_VERSION;
        public short maxDeviceMemory = 0;
        public short imageCacheSize = IMAGE_CACHE_SIZE;
        public byte resourceMemoryType = 0;
        public String[] exposedDeviceExtensions = null;
        public String libvulkanPath = null;
    }

    public VortekRendererComponent(Context context, XServer xServer, UnixSocketConfig socketConfig, Options options) {
        this.context = context.getApplicationContext();
        this.xServer = xServer;
        this.socketConfig = socketConfig;
        this.options = options != null ? options : new Options();

        initVulkanWrapper(this.context.getApplicationInfo().nativeLibraryDir, this.options.libvulkanPath);
    }

    @Override
    public void start() {
        if (connector != null) return;
        connector = new XConnectorEpoll(socketConfig, this, this);
        connector.setInitialInputBufferCapacity(8);
        connector.setInitialOutputBufferCapacity(0);
        connector.start();
    }

    @Override
    public void stop() {
        if (connector != null) {
            connector.stop();
            connector = null;
        }
    }

    @Keep
    private int getWindowWidth(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        return window != null ? window.getWidth() : 0;
    }

    @Keep
    private int getWindowHeight(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        return window != null ? window.getHeight() : 0;
    }

    @Keep
    private long getWindowHardwareBuffer(int windowId, boolean useHALPixelFormatBGRA8888) {
        Window window = xServer.windowManager.getWindow(windowId);
        if (window == null) return 0;

        Drawable drawable = window.getContent();
        if (drawable == null) return 0;

        // cmod Drawables are already backed by AHardwareBuffer. Reusing that buffer
        // avoids the upstream GPUImage replacement path and fits cmod's renderer model.
        return drawable.backingAHB;
    }

    @Keep
    private void updateWindowContent(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        if (window == null || window.getContent() == null) return;

        Drawable drawable = window.getContent();
        synchronized (drawable.renderLock) {
            Runnable onDrawListener = drawable.getOnDrawListener();
            if (onDrawListener != null) onDrawListener.run();
        }
    }

    @Override
    public void handleConnectionShutdown(Client client) {
        Object tag = client.getTag();
        if (tag instanceof Long) destroyVkContext((Long) tag);
    }

    @Override
    public void handleNewConnection(Client client) {
        client.createIOStreams();
    }

    @Override
    public boolean handleRequest(Client client) throws IOException {
        XInputStream inputStream = client.getInputStream();
        if (inputStream == null || inputStream.available() < 8) return false;

        int requestCode = inputStream.readInt();
        int requestLength = inputStream.readInt();

        if (requestCode == REQUEST_CODE_CREATE_CONTEXT) {
            long contextPtr = createVkContext(client.clientSocket.fd, options);
            if (contextPtr > 0) client.setTag(contextPtr);
            else connector.killConnection(client);
        }
        else if (requestCode > Short.MAX_VALUE && (requestCode >> 16) == REQUEST_CODE_SEND_EXTRA_DATA) {
            Object tag = client.getTag();
            if (!(tag instanceof Long)) throw new IOException("Vortek context is not initialized.");

            int requestId = requestCode & 0xffff;
            if (!handleExtraDataRequest((Long) tag, requestId, requestLength)) {
                throw new IOException("Failed to handle Vortek extra-data request.");
            }
        }

        return true;
    }

    private native long createVkContext(int clientFd, Options options);
    private native void destroyVkContext(long contextPtr);
    private native void initVulkanWrapper(String nativeLibraryDir, String libvulkanPath);
    private native boolean handleExtraDataRequest(long contextPtr, int requestCode, int requestLength);
}
