#include <jni.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
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

/* Must match vulkan_ahb.h on the Wine side */
#define MSG_PRESENT 1
#define MSG_RELEASE 2

struct present_msg {
    uint8_t  type;
    uint32_t slot_index;
    int32_t  acquire_fd;
    int32_t  dst_x, dst_y, dst_w, dst_h;
};

struct release_msg {
    uint8_t  type;         /* MSG_RELEASE */
    uint32_t slot_index;
    int32_t  release_fd;   /* -1 for now */
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
    pthread_t thread;
    pthread_t displayThread;
    int frameCount = 0;
    int prevSlotForRelease = -1;
};

static PresentReceiverState* g_presentReceiver = nullptr;

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
        state->renderer->scanoutSetBuffer(ahb, -1, slot,
            0, 0, state->screenWidth, state->screenHeight);
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

        /* Push to mailbox for display thread (never blocks) */
        state->mailboxSlot.store((int)slot, std::memory_order_release);

        /* Send immediate MSG_RELEASE for the previous slot so Wine can reuse it.
         * The display thread handles the actual (blocking) scanoutSetBuffer call. */
        if (state->prevSlotForRelease >= 0) {
            struct release_msg rel_msg;
            rel_msg.type = 2; /* MSG_RELEASE */
            rel_msg.slot_index = (uint32_t)state->prevSlotForRelease;
            rel_msg.release_fd = -1;
            send(state->clientFd, &rel_msg, sizeof(rel_msg), MSG_NOSIGNAL);
        }
        state->prevSlotForRelease = (int)slot;
    }

    /* Stop and join display thread */
    state->running.store(false);
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
