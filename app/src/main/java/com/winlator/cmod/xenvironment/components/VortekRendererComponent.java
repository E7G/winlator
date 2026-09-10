package com.winlator.cmod.xenvironment.components;

import android.content.Context;
import android.util.Log;

import androidx.annotation.Keep;

import com.winlator.cmod.contents.AdrenotoolsManager;
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

import java.io.File;
import java.io.IOException;
import java.util.HashMap;

/**
 * Host-side renderer service for the Vortek Vulkan ICD.
 *
 * The guest libvulkan_vortek.so connects to this component through an AF_UNIX
 * socket. The server then executes Vulkan work against either Android's system
 * Vulkan implementation or a driver loaded through libadrenotools.
 */
public final class VortekRendererComponent extends EnvironmentComponent
        implements ConnectionHandler, RequestHandler {
    private static final String TAG = "VortekRenderer";
    private static final byte REQUEST_CODE_CREATE_CONTEXT = 1;
    private static final byte REQUEST_CODE_SEND_EXTRA_DATA = 2;

    public static final String SERVER_PATH = "/tmp/.vortek/V0";
    public static final short IMAGE_CACHE_SIZE = 256;
    public static final int VK_MAX_VERSION = vkMakeVersion(1, 3, 128);

    private final XServer xServer;
    private final UnixSocketConfig socketConfig;
    private final Options options;
    private XConnectorEpoll connector;

    static {
        System.loadLibrary("vortekrenderer");
    }

    public static final class Options {
        public int vkMaxVersion = VK_MAX_VERSION;
        public short maxDeviceMemory = 0;
        public short imageCacheSize = IMAGE_CACHE_SIZE;
        public byte resourceMemoryType = 0;
        public String[] exposedDeviceExtensions = null;
        public String libvulkanPath = null;

        public static Options fromConfig(Context context, HashMap<String, String> config) {
            Options options = new Options();
            if (config == null) return options;

            String vulkanVersion = value(config, "vulkanVersion", "1.3");
            String[] parts = vulkanVersion.split("\\.");
            if (parts.length >= 2) {
                try {
                    options.vkMaxVersion = vkMakeVersion(
                            Integer.parseInt(parts[0]), Integer.parseInt(parts[1]), 128);
                } catch (NumberFormatException ignored) {
                    Log.w(TAG, "Invalid Vulkan version '" + vulkanVersion + "', using 1.3");
                }
            }

            options.maxDeviceMemory = parseShort(value(config, "maxDeviceMemory", "0"), (short) 0);
            options.imageCacheSize = parseShort(value(config, "imageCacheSize",
                    String.valueOf(IMAGE_CACHE_SIZE)), IMAGE_CACHE_SIZE);
            options.resourceMemoryType = mapResourceMemoryType(value(config, "resourceType", "auto"));

            // The existing cmod dialog stores an extension blacklist while Vortek expects an
            // allow-list. Passing null deliberately exposes all host-supported extensions and
            // avoids accidentally converting a blacklist into the inverse policy.
            options.exposedDeviceExtensions = null;

            String driverId = value(config, "version", "System");
            if (!driverId.isEmpty() && !"system".equalsIgnoreCase(driverId)) {
                AdrenotoolsManager manager = new AdrenotoolsManager(context);
                if (manager.isFromResources(driverId)) {
                    manager.extractDriverFromResources(driverId);
                }

                String libraryName = manager.getLibraryName(driverId);
                if (libraryName != null && !libraryName.isEmpty()) {
                    File driver = new File(manager.getDriverPath(driverId), libraryName);
                    if (driver.isFile()) {
                        options.libvulkanPath = driver.getAbsolutePath();
                        Log.i(TAG, "Using AdrenoTools Vulkan driver: " + driverId);
                    } else {
                        Log.w(TAG, "AdrenoTools driver library is missing for '" + driverId
                                + "'; falling back to system Vulkan");
                    }
                } else {
                    Log.w(TAG, "Unknown AdrenoTools driver '" + driverId
                            + "'; falling back to system Vulkan");
                }
            }
            return options;
        }

        private static String value(HashMap<String, String> config, String key, String fallback) {
            String value = config.get(key);
            return value == null || value.isEmpty() ? fallback : value;
        }

        private static short parseShort(String value, short fallback) {
            try {
                int parsed = Integer.parseInt(value);
                return (short) Math.max(0, Math.min(Short.MAX_VALUE, parsed));
            } catch (NumberFormatException ignored) {
                return fallback;
            }
        }

        private static byte mapResourceMemoryType(String value) {
            if (value == null) return 0;
            switch (value.toLowerCase()) {
                case "opaque":
                    return 1;
                case "dmabuf":
                    return 2;
                case "ahb":
                    return 3;
                default:
                    return 0;
            }
        }
    }

    public VortekRendererComponent(XServer xServer, UnixSocketConfig socketConfig, Options options) {
        this.xServer = xServer;
        this.socketConfig = socketConfig;
        this.options = options != null ? options : new Options();

        String nativeLibraryDir = xServer.getXServerView() != null
                ? xServer.getXServerView().getContext().getApplicationInfo().nativeLibraryDir
                : null;
        if (nativeLibraryDir == null) {
            throw new IllegalStateException("XServer renderer view is not initialized");
        }
        initVulkanWrapper(nativeLibraryDir, this.options.libvulkanPath);
    }

    @Override
    public void start() {
        if (connector != null) return;
        connector = new XConnectorEpoll(socketConfig, this, this);
        connector.setInitialInputBufferCapacity(8);
        connector.setInitialOutputBufferCapacity(0);
        connector.start();
        Log.i(TAG, "Vortek renderer server started at " + socketConfig.path);
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
        return drawable != null ? drawable.backingAHB : 0;
    }

    @Keep
    private void updateWindowContent(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        if (window == null) return;
        Drawable drawable = window.getContent();
        if (drawable == null) return;

        synchronized (drawable.renderLock) {
            drawable.updateDirect();
        }
        if (xServer.getXServerView() != null) xServer.getXServerView().requestRender();
    }

    @Override
    public void handleConnectionShutdown(Client client) {
        Object tag = client.getTag();
        if (tag instanceof Long) destroyVkContext((Long) tag);
    }

    @Override
    public void handleNewConnection(Client client) {
        // cmod's connector creates its streams lazily, unlike current upstream Winlator.
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
            if (contextPtr > 0) {
                client.setTag(contextPtr);
            } else if (connector != null) {
                connector.killConnection(client);
            }
        } else if (requestCode > Short.MAX_VALUE
                && (requestCode >> 16) == REQUEST_CODE_SEND_EXTRA_DATA) {
            int requestId = requestCode & 0xffff;
            Object tag = client.getTag();
            if (!(tag instanceof Long)) throw new IOException("Missing Vortek context");
            boolean success = handleExtraDataRequest((Long) tag, requestId, requestLength);
            if (!success) throw new IOException("Failed to handle Vortek extra-data request");
        }
        return true;
    }

    private static int vkMakeVersion(int major, int minor, int patch) {
        return (major << 22) | (minor << 12) | patch;
    }

    private native long createVkContext(int clientFd, Options options);
    private native void destroyVkContext(long contextPtr);
    private native void initVulkanWrapper(String nativeLibraryDir, String libvulkanPath);
    private native boolean handleExtraDataRequest(long contextPtr, int requestCode, int requestLength);
}
