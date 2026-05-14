#include <jni.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <android/looper.h>
#include <android/choreographer.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <android/log.h>
#include "../adrenotools/include/adrenotools/driver.h"
#include "VulkanRendererContext.h"

#define PRESENT_LOG_TAG "AHB_PresentRecv"
#define PRESENT_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PRESENT_LOG_TAG, __VA_ARGS__)
#define PRESENT_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PRESENT_LOG_TAG, __VA_ARGS__)

/* Must match vulkan_ahb.h on the Wine side — protocol break if layouts diverge. */
#define MSG_PRESENT 1
#define MSG_RELEASE 2
#define MSG_TICK    5
#define MSG_VSYNC   6

struct present_msg {
    uint8_t  type;
    uint32_t slot_index;
    int32_t  acquire_fd;
    int32_t  dst_x, dst_y, dst_w, dst_h;
    uint64_t present_id;   /* DXVK's VkPresentIdKHR.pPresentIds[i]; 0 if none */
    uint8_t  bgra_bytes;   /* 1 = source AHB has BGRA byte order; do an R↔B
                            * swap during the receiver's local blit (via
                            * vkCmdBlitImage with B8G8R8A8 src + R8G8B8A8 dst).
                            * 0 = AHB already has RGBA bytes; plain copy. */
};

struct release_msg {
    uint8_t  type;         /* MSG_RELEASE / MSG_TICK / MSG_VSYNC */
    uint32_t slot_index;
    int32_t  release_fd;   /* -1 for now */
    uint8_t  displayed;    /* 1 = onComplete (frame shown), 0 = mailbox drop */
    uint64_t vsync_time_ns; /* For MSG_VSYNC: AChoreographer frameTimeNanos.
                             * Carries the real panel vsync timestamp so the
                             * Wine-side layer can phase-anchor its wake
                             * target on the actual vsync, not on the IPC
                             * arrival time (which has thread-scheduling
                             * variance). Ignored for non-VSYNC types. */
};

/* State for the native present-receiver thread */
struct PresentReceiverState {
    VulkanRendererContext* renderer;
    int clientFd;
    AHardwareBuffer* ahbSlots[4];
    int slotCount;
    int screenWidth;
    int screenHeight;
    std::atomic<bool> running{true};
    std::atomic<int> mailboxSlot{-1};  /* latest slot for display thread */
    /* Sticky AHB byte-order flag, set by the recv thread from each
     * present_msg.bgra_bytes. The layer's mode (direct-render vs
     * trojan-blit) is pinned per swapchain, so this value is stable for
     * the lifetime of the connection — recv writes it on every frame,
     * display reads the latest value when it picks up a slot. */
    std::atomic<int> sourceBgraBytes{0};
    pthread_t thread;
    pthread_t displayThread;
    /* Phase-lock vsync source. Spawned at receiver-startup, it runs an
     * AChoreographer callback loop on a dedicated thread; each panel
     * vsync emits MSG_VSYNC on the same Wine IPC socket as MSG_RELEASE.
     * The layer's release-reader thread consumes both, advancing a
     * phase-locked counter that drives vkWaitForPresentKHR. */
    pthread_t vsyncThread;
    bool vsyncThreadStarted = false;
    ALooper* vsyncLooper = nullptr;       /* owned by vsyncThread, woken via ALooper_wake */
    AChoreographer* vsyncChoreographer = nullptr;
    int frameCount = 0;
    int prevSlotForRelease = -1;
};

static PresentReceiverState* g_presentReceiver = nullptr;

/* AChoreographer frame callback. Fires once per real panel vsync. We send
 * a single MSG_VSYNC to the Wine layer over the existing IPC socket and
 * immediately re-register for the next vsync.
 *
 * Notes:
 *   - send() is non-blocking (MSG_DONTWAIT) so a stuck Wine socket can NEVER
 *     stall this callback. Vsync delivery is more important than guaranteed
 *     delivery; if a packet is dropped, the layer will simply pace one
 *     vsync late on the next iteration, which is recoverable.
 *   - We re-post the callback from inside the callback (Choreographer is
 *     one-shot per registration) to keep the stream alive.
 *   - The callback runs on the vsyncThread, which has its own Looper. We
 *     access only state->clientFd (atomic-stable int) and the Choreographer
 *     instance; no other state mutation. */
static void vsync_frame_callback(long frameTimeNanos, void* data);

static void vsync_frame_callback(long frameTimeNanos, void* data) {
    auto* state = reinterpret_cast<PresentReceiverState*>(data);
    if (!state || !state->running.load()) return;

    /* Send MSG_VSYNC on the IPC socket. release_msg layout, with
     * vsync_time_ns carrying AChoreographer's frameTimeNanos — that's the
     * REAL panel vsync timestamp in CLOCK_MONOTONIC ns. Putting it in the
     * payload lets the Wine-side layer phase-anchor its wake target on
     * the actual vsync moment rather than on its own recv time, which
     * removes thread-scheduling latency from the anchor. */
    struct release_msg msg{};
    msg.type = (uint8_t)MSG_VSYNC;
    msg.slot_index = 0;
    msg.release_fd = -1;
    msg.displayed = 0;
    msg.vsync_time_ns = (uint64_t)frameTimeNanos;
    int fd = state->clientFd;
    if (fd >= 0) {
        (void)send(fd, &msg, sizeof(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
    }

    /* Re-register for the next vsync. */
    if (state->vsyncChoreographer) {
        AChoreographer_postFrameCallback(state->vsyncChoreographer,
                                         vsync_frame_callback, data);
    }
}

/* Choreographer thread: owns the Looper that hosts AChoreographer callbacks.
 * AChoreographer requires a Looper-attached thread to call. We use a
 * dedicated thread so the recv thread isn't blocked driving its own loop. */
static void* vsync_thread_func(void* arg) {
    auto* state = reinterpret_cast<PresentReceiverState*>(arg);
    PRESENT_LOGI("vsync thread started (Choreographer phase-lock source)");

    /* Prepare a Looper on this thread. ALOOPER_PREPARE_ALLOW_NON_CALLBACKS=0
     * because we'll use AChoreographer_postFrameCallback which DOES use
     * callbacks via the Choreographer's internal fd. */
    state->vsyncLooper = ALooper_prepare(0);
    if (!state->vsyncLooper) {
        PRESENT_LOGE("vsync thread: ALooper_prepare failed; phase-lock disabled");
        return nullptr;
    }
    /* Increment refcount so the Looper survives across thread teardown. */
    ALooper_acquire(state->vsyncLooper);

    state->vsyncChoreographer = AChoreographer_getInstance();
    if (!state->vsyncChoreographer) {
        PRESENT_LOGE("vsync thread: AChoreographer_getInstance failed; phase-lock disabled");
        return nullptr;
    }

    /* Register the first callback. From here it's self-sustaining: each
     * callback re-posts itself. */
    AChoreographer_postFrameCallback(state->vsyncChoreographer,
                                     vsync_frame_callback, state);
    PRESENT_LOGI("vsync thread: first Choreographer callback posted");

    /* Pump the Looper. ALooper_pollOnce is the supported successor to
     * ALooper_pollAll (which the NDK marks obsoleted because it could
     * silently swallow wakeups). pollOnce returns after one event/wake/
     * timeout; we just loop on it. Stops when running goes false; we
     * wake it from the receiver shutdown path via ALooper_wake. */
    while (state->running.load()) {
        int rc = ALooper_pollOnce(-1 /* block indefinitely */,
                                  nullptr, nullptr, nullptr);
        if (rc == ALOOPER_POLL_ERROR) {
            PRESENT_LOGE("vsync thread: ALooper_pollOnce returned ERROR; exiting");
            break;
        }
        /* ALOOPER_POLL_WAKE (-1) or ALOOPER_POLL_CALLBACK (-2) — re-check
         * running and loop. */
    }

    ALooper_release(state->vsyncLooper);
    state->vsyncLooper = nullptr;
    state->vsyncChoreographer = nullptr;
    PRESENT_LOGI("vsync thread exiting");
    return nullptr;
}

/* Display thread: pulls from mailbox, calls scanoutSetBuffer (may block on vsync) */
static void* display_thread_func(void* arg) {
    auto* state = reinterpret_cast<PresentReceiverState*>(arg);
    PRESENT_LOGI("display thread started");
    bool scanoutInitialized = false;

    while (state->running.load()) {
        int slot = state->mailboxSlot.exchange(-1, std::memory_order_acquire);
        if (slot < 0) {
            usleep(1000); /* 1ms poll — no frame pending */
            continue;
        }
        AHardwareBuffer* ahb = state->ahbSlots[slot];
        if (!ahb) continue;

        if (!scanoutInitialized) {
            scanoutInitialized = true;
            if (!state->renderer->scanoutActive.load()) {
                state->renderer->initScanout();
                PRESENT_LOGI("display thread: initScanout done");
            }
        }
        int bgraBytes = state->sourceBgraBytes.load(std::memory_order_acquire);
        state->renderer->scanoutSetBuffer(ahb, -1, slot,
            0, 0, state->screenWidth, state->screenHeight, bgraBytes);
        /* Trigger the GPU blit + SurfaceFlinger submission immediately.
         * applyScanoutBuffer() blits from the source AHB to a LOCAL display buffer,
         * then submits the LOCAL buffer to SurfaceFlinger. This means the source AHB
         * is freed immediately after the blit (~1ms), preventing the gralloc lock
         * from blocking Wine's GPU rendering. */
        state->renderer->applyScanoutBuffer();
    }
    PRESENT_LOGI("display thread exiting");
    return nullptr;
}

static void* present_receiver_thread(void* arg) {
    auto* state = reinterpret_cast<PresentReceiverState*>(arg);
    PRESENT_LOGI("thread started: fd=%d, slots=%d, renderer=%p",
        state->clientFd, state->slotCount, (void*)state->renderer);

    /* Store socket fd in renderer for onComplete callback */
    state->renderer->scanoutSocketFd.store(state->clientFd, std::memory_order_relaxed);

    /* Start the display thread */
    pthread_create(&state->displayThread, nullptr, display_thread_func, state);

    /* Start the AChoreographer-driven vsync thread. This emits MSG_VSYNC
     * on the same IPC socket once per real panel refresh and is what the
     * layer's vkWaitForPresentKHR phase-locks against to eliminate
     * scheduler-jitter "doubled motion" during fast camera motion. The
     * layer has a wall-clock fallback for the brief startup window before
     * the first MSG_VSYNC arrives, so failure here is non-fatal. */
    if (pthread_create(&state->vsyncThread, nullptr, vsync_thread_func, state) == 0) {
        state->vsyncThreadStarted = true;
    } else {
        PRESENT_LOGE("recv thread: failed to start vsync thread — pacing falls back to wall-clock");
    }

    /* initScanout is called lazily on FIRST present_msg so SurfaceControl
     * layers are NOT created until we know real AHB frames will arrive.
     * This prevents SC layers from covering the X11 display when the AHB
     * swapchain creation fails (e.g., LD_PRELOAD dispatch table not patched). */
    bool scanoutInitialized = false;

    while (state->running.load()) {
        struct present_msg pmsg;
        struct msghdr msg = {};
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];

        iov.iov_base = &pmsg;
        iov.iov_len = sizeof(pmsg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t ret = recvmsg(state->clientFd, &msg, 0);
        if (ret <= 0) {
            if (ret == 0) {
                PRESENT_LOGI("thread: Wine disconnected (EOF)");
            } else {
                PRESENT_LOGE("thread: recvmsg failed: %s", strerror(errno));
            }
            break;
        }

        if ((size_t)ret < sizeof(pmsg)) {
            PRESENT_LOGE("thread: short read (%zd < %zu)", ret, sizeof(pmsg));
            continue;
        }

        if (pmsg.type != MSG_PRESENT) {
            PRESENT_LOGI("thread: ignoring non-present msg type=%d", pmsg.type);
            continue;
        }

        /* Extract acquire fence fd from ancillary data if present */
        int acquireFd = -1;
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&acquireFd, CMSG_DATA(cmsg), sizeof(int));
        }

        uint32_t slot = pmsg.slot_index;
        if (slot >= (uint32_t)state->slotCount) {
            PRESENT_LOGE("thread: invalid slot %u (max=%d)", slot, state->slotCount);
            if (acquireFd >= 0) close(acquireFd);
            continue;
        }

        AHardwareBuffer* ahb = state->ahbSlots[slot];
        if (!ahb) {
            PRESENT_LOGE("thread: slot %u has null AHB", slot);
            if (acquireFd >= 0) close(acquireFd);
            continue;
        }

        state->frameCount++;
        if (state->frameCount <= 5 || (state->frameCount % 60 == 0))
            PRESENT_LOGI("thread: received slot=%u acquireFd=%d frame=%d", slot, acquireFd, state->frameCount);

        /* initScanout is now handled by the display thread */
        (void)scanoutInitialized;

        /* Drain any additional pending messages before submitting to SurfaceFlinger.
         * This implements "mailbox" semantics — only the LATEST frame is displayed,
         * intermediate frames are dropped. This prevents scanoutSetBuffer from
         * blocking when SurfaceFlinger's buffer queue is full. */
        {
            struct pollfd pfd = { .fd = state->clientFd, .events = POLLIN };
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                /* There's more data — read the next message and use it instead */
                struct present_msg next_pmsg;
                struct iovec next_iov = { .iov_base = &next_pmsg, .iov_len = sizeof(next_pmsg) };
                char next_cmsg_buf[CMSG_SPACE(sizeof(int))];
                struct msghdr next_msg = {};
                next_msg.msg_iov = &next_iov;
                next_msg.msg_iovlen = 1;
                next_msg.msg_control = next_cmsg_buf;
                next_msg.msg_controllen = sizeof(next_cmsg_buf);
                ssize_t next_ret = recvmsg(state->clientFd, &next_msg, MSG_DONTWAIT);
                if (next_ret == (ssize_t)sizeof(next_pmsg) && next_pmsg.type == MSG_PRESENT
                    && next_pmsg.slot_index < (uint32_t)state->slotCount) {
                    /* MAILBOX DROP: this frame is being skipped in favor of a newer one.
                     * Send MSG_RELEASE for the dropped slot immediately so Wine can reuse
                     * it. displayed=0 tells the layer this release was a drop, not an
                     * actual display tick — the layer must NOT advance its vsync-paced
                     * display counter (used for vkWaitForPresentKHR). */
                    {
                        struct release_msg drop_rel{};  /* zero-init vsync_time_ns */
                        drop_rel.type = MSG_RELEASE;
                        drop_rel.slot_index = slot;
                        drop_rel.release_fd = -1;
                        drop_rel.displayed = 0;
                        send(state->clientFd, &drop_rel, sizeof(drop_rel), MSG_NOSIGNAL);
                    }
                    /* Drop the old frame, use this newer one */
                    if (acquireFd >= 0) close(acquireFd);
                    slot = next_pmsg.slot_index;
                    ahb = state->ahbSlots[slot];
                    acquireFd = -1;
                    /* Extract acquire fence from drained message */
                    struct cmsghdr* nc = CMSG_FIRSTHDR(&next_msg);
                    if (nc && nc->cmsg_level == SOL_SOCKET && nc->cmsg_type == SCM_RIGHTS) {
                        memcpy(&acquireFd, CMSG_DATA(nc), sizeof(int));
                    }
                    state->frameCount++;
                } else {
                    /* Close any leaked fd from a non-matching drained message */
                    struct cmsghdr* nc = CMSG_FIRSTHDR(&next_msg);
                    if (nc && nc->cmsg_level == SOL_SOCKET && nc->cmsg_type == SCM_RIGHTS) {
                        int leaked_fd;
                        memcpy(&leaked_fd, CMSG_DATA(nc), sizeof(int));
                        close(leaked_fd);
                    }
                    break;
                }
            }
        }

        /* Record the source AHB byte order from the layer (BGRA in
         * direct-render mode, RGBA in trojan-blit mode). The display
         * thread reads this when it picks up the next slot and tells the
         * compositor whether to do an R↔B swap during the local blit. */
        state->sourceBgraBytes.store(pmsg.bgra_bytes != 0 ? 1 : 0,
                                     std::memory_order_release);

        /* Push the LATEST slot to the mailbox for the display thread. With true
         * mailbox semantics, Wine renders unthrottled up to the pool capacity; the
         * drain loop above sends immediate releases for skipped frames, and the
         * onComplete callback in applyScanoutBuffer sends the release for the
         * actually-displayed frame at vsync. The natural back-pressure is Wine
         * waiting for the pool to refill, not for vsync. */
        state->mailboxSlot.store((int)slot, std::memory_order_release);
        state->prevSlotForRelease = (int)slot;
    }

    /* Stop and join display + vsync threads.
     *
     * Order: set running=false first so both threads notice on their next
     * iteration. The vsync thread is blocked in ALooper_pollAll(-1) — wake
     * it explicitly so it can re-check the running flag. */
    state->running.store(false);
    if (state->vsyncThreadStarted) {
        if (state->vsyncLooper) ALooper_wake(state->vsyncLooper);
        pthread_join(state->vsyncThread, nullptr);
        state->vsyncThreadStarted = false;
    }
    pthread_join(state->displayThread, nullptr);
    PRESENT_LOGI("thread exiting");
    return nullptr;
}

static void* openAdrenotoolsDriver(const char* driverPath, const char* libraryName, const char* nativeLibDir) {
    if (!driverPath || !libraryName || !nativeLibDir) return nullptr;
    if (access(driverPath, F_OK) != 0) {
        __android_log_print(ANDROID_LOG_ERROR,"Winlator_Renderer",
            "openAdrenotoolsDriver: driverPath not accessible: %s", driverPath);
        return nullptr;
    }
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%stemp", driverPath);
    mkdir(tmpdir, S_IRWXU | S_IRWXG);
    __android_log_print(ANDROID_LOG_DEBUG,"Winlator_Renderer",
        "openAdrenotoolsDriver: driverPath=%s lib=%s nativeLibDir=%s tmp=%s",
        driverPath, libraryName, nativeLibDir, tmpdir);
    setenv("ADRENOTOOLS_DRIVER_PATH", driverPath, 1);
    setenv("ADRENOTOOLS_DRIVER_NAME", libraryName, 1);
    setenv("ADRENOTOOLS_HOOKS_PATH", nativeLibDir, 1);
    const char* redirectDir = getenv("ADRENOTOOLS_REDIRECT_DIR");
    int featureFlags = ADRENOTOOLS_DRIVER_CUSTOM;
    if (redirectDir && redirectDir[0] != '\0') {
        featureFlags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;
    } else {
        unsetenv("ADRENOTOOLS_DRIVER_FILE_REDIRECT");
    }
    void* handle = adrenotools_open_libvulkan(
        RTLD_LOCAL | RTLD_NOW,
        featureFlags,
        tmpdir,
        nativeLibDir,
        driverPath,
        libraryName,
        (redirectDir && redirectDir[0] != '\0') ? redirectDir : nullptr,
        nullptr);
    if (!handle) {
        __android_log_print(ANDROID_LOG_ERROR,"Winlator_Renderer",
            "openAdrenotoolsDriver: adrenotools_open_libvulkan failed");
    } else {
        __android_log_print(ANDROID_LOG_DEBUG,"Winlator_Renderer",
            "openAdrenotoolsDriver: SUCCESS handle=%p", handle);
    }
    return handle;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeInit(
    JNIEnv* env, jobject, jobject surface, jint w, jint h,
    jstring jDriverPath, jstring jLibraryName, jstring jNativeLibDir)
{
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (!win) return 0;
    void* adrenotoolsHandle = nullptr;
    if (jDriverPath && jLibraryName && jNativeLibDir) {
        const char* dp  = env->GetStringUTFChars(jDriverPath,   nullptr);
        const char* lib = env->GetStringUTFChars(jLibraryName,  nullptr);
        const char* nld = env->GetStringUTFChars(jNativeLibDir, nullptr);
        adrenotoolsHandle = openAdrenotoolsDriver(dp, lib, nld);
        env->ReleaseStringUTFChars(jDriverPath,   dp);
        env->ReleaseStringUTFChars(jLibraryName,  lib);
        env->ReleaseStringUTFChars(jNativeLibDir, nld);
    }
    try { return reinterpret_cast<jlong>(new VulkanRendererContext(win, w, h, adrenotoolsHandle)); }
    catch (...) {
        ANativeWindow_release(win);
        if (adrenotoolsHandle) dlclose(adrenotoolsHandle);
        return 0;
    }
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeResize(JNIEnv*, jobject, jlong h, jint w, jint ht) {
    auto* r=reinterpret_cast<VulkanRendererContext*>(h); if (r) r->onSurfaceResized(w,ht);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeDestroy(JNIEnv*, jobject, jlong h) {
    delete reinterpret_cast<VulkanRendererContext*>(h);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeUpdateWindowContent(
    JNIEnv* env, jobject, jlong handle, jlong id, jobject buf, jshort w, jshort h, jshort stride, jint x, jint y)
{
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r||!buf) return;
    void* px=env->GetDirectBufferAddress(buf);
    if (px && env->GetDirectBufferCapacity(buf)>=(jlong)w*h*4)
        r->updateWindowContent(id,px,w,h,stride,x,y);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeUpdateWindowContentAHB(
    JNIEnv*, jobject, jlong handle, jlong id, jlong ahbPtr, jshort w, jshort h, jint x, jint y)
{
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle);
    if (r&&ahbPtr) r->updateWindowContentAHB(id,reinterpret_cast<AHardwareBuffer*>(ahbPtr),w,h,x,y);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetTransform(
    JNIEnv*, jobject, jlong handle, jfloat ox, jfloat oy, jfloat sx, jfloat sy)
{
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle); if (r) r->setTransform(ox,oy,sx,sy);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetPointerPos(JNIEnv*, jobject, jlong handle, jshort x, jshort y) {
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle); if (r) r->updatePointerPosition(x,y);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetCursorVisible(JNIEnv*, jobject, jlong handle, jboolean v) {
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle); if (r) r->setCursorVisible(v);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeUpdateCursorImage(
    JNIEnv* env, jobject, jlong handle, jobject buf, jshort w, jshort h, jshort hotX, jshort hotY)
{
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r||!buf) return;
    void* px=env->GetDirectBufferAddress(buf);
    if (px && env->GetDirectBufferCapacity(buf)>=(jlong)w*h*4)
        r->updateCursorImage(px,w,h,hotX,hotY);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetRenderList(
    JNIEnv* env, jobject, jlong handle, jlongArray jids, jintArray jxs, jintArray jys, jint count)
{
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r||count<=0) return;
    jlong* ids=env->GetLongArrayElements(jids,nullptr);
    jint*  xs =env->GetIntArrayElements(jxs,nullptr);
    jint*  ys =env->GetIntArrayElements(jys,nullptr);
    r->setRenderList(reinterpret_cast<const int64_t*>(ids),xs,ys,count);
    env->ReleaseLongArrayElements(jids,ids,JNI_ABORT);
    env->ReleaseIntArrayElements(jxs,xs,JNI_ABORT);
    env->ReleaseIntArrayElements(jys,ys,JNI_ABORT);
}
extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeRemoveWindow(JNIEnv*, jobject, jlong handle, jlong id) {
    auto* r=reinterpret_cast<VulkanRendererContext*>(handle); if (r) r->removeWindow(id);
}




extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeInitScanout(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->initScanout();
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeDestroyScanout(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->destroyScanout();
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeScanoutSetBuffer(
    JNIEnv*, jobject, jlong handle, jlong ahbPtr, jint acquireFenceFd, jint x, jint y, jint w, jint h)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r && ahbPtr) r->scanoutSetBuffer(reinterpret_cast<AHardwareBuffer*>(ahbPtr), (int)acquireFenceFd, -1, x, y, w, h);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativePollReleaseFence(
    JNIEnv* env, jobject, jlong handle)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r) return nullptr;
    auto [slotIndex, releaseFd] = r->pollReleaseFence();
    if (slotIndex < 0) return nullptr;
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    jint buf[2] = { (jint)slotIndex, (jint)releaseFd };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeScanoutSetCursorImage(
    JNIEnv* env, jobject, jlong handle, jobject buf, jshort w, jshort h, jshort stride)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r || !buf) return;
    void* px = env->GetDirectBufferAddress(buf);
    if (px && env->GetDirectBufferCapacity(buf) >= (jlong)w*h*4)
        r->scanoutSetCursorImage(px, w, h, stride);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeScanoutSetCursorPos(
    JNIEnv*, jobject, jlong handle, jshort x, jshort y, jshort hotX, jshort hotY)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->scanoutSetCursorPos(x, y, hotX, hotY);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeIsScanoutActive(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    return r ? (jboolean)r->scanoutActive.load() : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeScanoutSetDst(
    JNIEnv*, jobject, jlong handle, jint x, jint y, jint w, jint h)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->scanoutSetDst(x, y, w, h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetScanoutWindow(
    JNIEnv* env, jobject, jlong handle, jobject gameSurface, jobject cursorSurface)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r) return;
    ANativeWindow* gw = ANativeWindow_fromSurface(env, gameSurface);
    ANativeWindow* cw = ANativeWindow_fromSurface(env, cursorSurface);
    if (!gw || !cw) {
        if (gw) ANativeWindow_release(gw);
        if (cw) ANativeWindow_release(cw);
        r->initScanout();
        return;
    }
    r->initScanoutFromWindows(gw, cw);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetVerboseLog(JNIEnv*, jobject, jlong handle, jboolean v) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->setVerboseLog((bool)v);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeDumpRendererInfo(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->dumpRendererInfo();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeIsGameFrameDelivered(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    return r ? (jboolean)r->gameFrameDelivered.load() : JNI_FALSE;
}

/* Counter of frames delivered via the DAC direct-compositing path. Polled by
 * the HUD overlay each second to compute DAC-mode FPS. Returns 0 if the
 * renderer handle isn't set up yet (e.g., very early in startup). */
extern "C" JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeGetDirectFrameCount(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    return r ? (jlong)r->directFrameCount.load(std::memory_order_relaxed) : 0L;
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetFilterMode(JNIEnv*, jobject, jlong handle, jint mode) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->setFilterMode((int)mode);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetSwapRB(JNIEnv*, jobject, jlong handle, jboolean enabled) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->setSwapRB(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetPresentMode(JNIEnv*, jobject, jlong handle, jint mode) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->setPresentMode((VkPresentModeKHR)mode);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeSetEffect(JNIEnv*, jobject, jlong handle, jint effectId, jfloat sharpness) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->setEffect((int)effectId, (float)sharpness);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeDetachSurface(JNIEnv*, jobject, jlong handle) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (r) r->detachSurface();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeReattachSurface(JNIEnv* env, jobject, jlong handle, jobject surface) {
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r || !surface) return JNI_FALSE;
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (!win) return JNI_FALSE;
    bool ok = r->reattachSurface(win);
    if (ok && r->scanoutActive.load()) {
        r->destroyScanout();
    }
    return (jboolean)ok;
}


/* ========================================================================
 * Native present-receiver thread JNI interface
 *
 * Called by AHBSocketServerComponent after Wine connects and AHBs are sent.
 * This thread reads present_msg from the Wine client socket and calls
 * scanoutSetBuffer() directly, bypassing XConnectorEpoll.
 * ======================================================================== */

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeStartPresentReceiver(
    JNIEnv* env, jobject, jlong rendererHandle, jint clientFd,
    jlongArray ahbPtrs, jint screenWidth, jint screenHeight)
{
    (void)env; (void)ahbPtrs; // unused — use nativeStartPresentReceiverWithSlots instead
    auto* renderer = reinterpret_cast<VulkanRendererContext*>(rendererHandle);
    if (!renderer || clientFd < 0) {
        PRESENT_LOGE("nativeStartPresentReceiver: invalid args (renderer=%p, fd=%d)",
            (void*)renderer, clientFd);
        return;
    }

    /* Stop any existing receiver */
    if (g_presentReceiver) {
        g_presentReceiver->running.store(false);
        shutdown(g_presentReceiver->clientFd, SHUT_RDWR);
        pthread_join(g_presentReceiver->thread, nullptr);
        delete g_presentReceiver;
        g_presentReceiver = nullptr;
    }

    auto* state = new PresentReceiverState();
    state->renderer = renderer;
    state->clientFd = clientFd;
    state->screenWidth = screenWidth;
    state->screenHeight = screenHeight;
    state->slotCount = 0;
    state->ahbSlots[0] = state->ahbSlots[1] = state->ahbSlots[2] = state->ahbSlots[3] = nullptr;

    PRESENT_LOGI("nativeStartPresentReceiver: renderer=%p fd=%d screen=%dx%d (no slots)",
        (void*)renderer, clientFd, screenWidth, screenHeight);

    /* Activate nativeMode + scanout before first frame arrives */
    if (!renderer->scanoutActive.load()) {
        renderer->initScanout();
        PRESENT_LOGI("nativeStartPresentReceiver: initScanout called");
    }

    g_presentReceiver = state;

    pthread_create(&state->thread, nullptr, present_receiver_thread, state);
    pthread_setname_np(state->thread, "ahb_present_rx");
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeStartPresentReceiverWithSlots(
    JNIEnv*, jobject, jlong rendererHandle, jint clientFd,
    jlong ahb0, jlong ahb1, jlong ahb2, jlong ahb3,
    jint screenWidth, jint screenHeight)
{
    auto* renderer = reinterpret_cast<VulkanRendererContext*>(rendererHandle);
    if (!renderer || clientFd < 0) {
        PRESENT_LOGE("nativeStartPresentReceiverWithSlots: invalid args");
        return;
    }

    /* Stop any existing receiver */
    if (g_presentReceiver) {
        g_presentReceiver->running.store(false);
        shutdown(g_presentReceiver->clientFd, SHUT_RDWR);
        pthread_join(g_presentReceiver->thread, nullptr);
        delete g_presentReceiver;
        g_presentReceiver = nullptr;
    }

    auto* state = new PresentReceiverState();
    state->renderer = renderer;
    state->clientFd = clientFd;
    state->screenWidth = screenWidth;
    state->screenHeight = screenHeight;
    state->ahbSlots[0] = reinterpret_cast<AHardwareBuffer*>(ahb0);
    state->ahbSlots[1] = reinterpret_cast<AHardwareBuffer*>(ahb1);
    state->ahbSlots[2] = reinterpret_cast<AHardwareBuffer*>(ahb2);
    state->ahbSlots[3] = reinterpret_cast<AHardwareBuffer*>(ahb3);
    state->slotCount = (ahb3 != 0) ? 4 : 3;

    PRESENT_LOGI("nativeStartPresentReceiverWithSlots: renderer=%p fd=%d slots=[%p,%p,%p,%p] count=%d screen=%dx%d",
        (void*)renderer, clientFd,
        (void*)state->ahbSlots[0], (void*)state->ahbSlots[1],
        (void*)state->ahbSlots[2], (void*)state->ahbSlots[3],
        state->slotCount, screenWidth, screenHeight);

    /* NOTE: initScanout() is now deferred to the first present_msg so that
     * SurfaceControl layers are NOT created until we confirm AHB frames
     * will actually arrive.  Creating SC layers here (before AHB swapchain
     * creation) caused the X11 display to be covered by empty SC layers,
     * resulting in a permanent black screen when DXVK used the real Vulkan
     * swapchain instead of the AHB one.
     *
     * The lazy init happens inside present_receiver_thread on the first
     * MSG_PRESENT message. */

    g_presentReceiver = state;

    pthread_create(&state->thread, nullptr, present_receiver_thread, state);
    pthread_setname_np(state->thread, "ahb_present_rx");
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_cmod_renderer_VulkanRenderer_nativeStopPresentReceiver(
    JNIEnv*, jobject, jlong rendererHandle)
{
    if (g_presentReceiver) {
        PRESENT_LOGI("nativeStopPresentReceiver: stopping thread");
        g_presentReceiver->running.store(false);
        shutdown(g_presentReceiver->clientFd, SHUT_RDWR);
        pthread_join(g_presentReceiver->thread, nullptr);
        delete g_presentReceiver;
        g_presentReceiver = nullptr;
    }
}
