package com.winlator.cmod.xenvironment.components;

import android.util.Log;

import com.winlator.cmod.renderer.AHardwareBufferPool;
import com.winlator.cmod.renderer.VulkanRenderer;
import com.winlator.cmod.xconnector.Client;
import com.winlator.cmod.xconnector.ConnectionHandler;
import com.winlator.cmod.xconnector.RequestHandler;
import com.winlator.cmod.xconnector.UnixSocketConfig;
import com.winlator.cmod.xconnector.XConnectorEpoll;
import com.winlator.cmod.xenvironment.EnvironmentComponent;

/**
 * Wine/DXVK <-> Android AHB transport. Once the handshake is complete, the
 * client fd is removed from Java epoll and owned by DirectAHBCompositor's
 * native receiver thread.
 */
public class AHBSocketServerComponent extends EnvironmentComponent
        implements ConnectionHandler, RequestHandler {
    public static final String AHB_SOCKET_PATH = "/usr/tmp/.ahb/AHB0";
    private static final String TAG = "AHBSocket";

    private final UnixSocketConfig socketConfig;
    private final DirectCompositorComponent compositor;
    private XConnectorEpoll connector;
    private volatile boolean nativeReceiverOwnsClient;

    public AHBSocketServerComponent(UnixSocketConfig socketConfig,
                                    DirectCompositorComponent compositor) {
        this.socketConfig = socketConfig;
        this.compositor = compositor;
    }

    @Override
    public void start() {
        if (!compositor.isReady()) return;
        try {
            connector = new XConnectorEpoll(socketConfig, this, this);
            connector.setCanReceiveAncillaryMessages(true);
            connector.setMultithreadedClients(false);
            connector.start();
            Log.i(TAG, "listening on " + socketConfig.path);
        } catch (Exception e) {
            Log.e(TAG, "failed to start AHB server", e);
        }
    }

    @Override
    public void stop() {
        compositor.getRenderer().stopDirectAHBReceiver();
        nativeReceiverOwnsClient = false;
        if (connector != null) {
            connector.stop();
            connector = null;
        }
    }

    @Override
    public void handleNewConnection(Client client) {
        AHardwareBufferPool pool = compositor.getPool();
        if (pool == null) return;

        for (int i = 0; i < pool.getCount(); i++) {
            long ptr = pool.getBufferPtr(i);
            if (ptr == 0 || AHardwareBufferPool.nativeSendBufferToSocket(ptr, client.clientSocket.fd) < 0) {
                Log.e(TAG, "failed to send AHB slot " + i);
                nativeReceiverOwnsClient = false;
                return;
            }
        }

        long b0 = pool.getBufferPtr(0);
        long b1 = pool.getBufferPtr(1);
        long b2 = pool.getBufferPtr(2);
        long b3 = pool.getCount() > 3 ? pool.getBufferPtr(3) : 0L;
        VulkanRenderer renderer = compositor.getRenderer();
        boolean started = renderer.startDirectAHBReceiver(
                client.clientSocket.fd, b0, b1, b2, b3, pool.getCount(),
                pool.getWidth(), pool.getHeight(), compositor.getRefreshRate());

        nativeReceiverOwnsClient = started;
        if (started && connector != null) {
            connector.detachClientFromEpoll(client);
            Log.i(TAG, "native DirectAHB receiver owns fd=" + client.clientSocket.fd);
        } else {
            Log.w(TAG, "DirectAHB startup failed; connection will be closed and Vulkan fallback kept");
        }
    }

    @Override
    public void handleConnectionShutdown(Client client) {
        if (!nativeReceiverOwnsClient) compositor.getRenderer().stopDirectAHBReceiver();
        nativeReceiverOwnsClient = false;
    }

    @Override
    public boolean handleRequest(Client client) {
        // If native startup failed, let XConnectorEpoll close this connection;
        // the Vulkan layer then falls back instead of feeding an unread socket.
        return nativeReceiverOwnsClient;
    }
}
