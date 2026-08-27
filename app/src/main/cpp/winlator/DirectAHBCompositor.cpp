#include "DirectAHBCompositor.h"

#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#define DAC_TAG "Winlator_DirectAHB"
#define DLOGI(...) __android_log_print(ANDROID_LOG_INFO, DAC_TAG, __VA_ARGS__)
#define DLOGW(...) __android_log_print(ANDROID_LOG_WARN, DAC_TAG, __VA_ARGS__)
#define DLOGE(...) __android_log_print(ANDROID_LOG_ERROR, DAC_TAG, __VA_ARGS__)

namespace {
constexpr uint8_t MSG_PRESENT = 1;
constexpr uint8_t MSG_RELEASE = 2;
constexpr uint8_t MSG_TICK = 5;
constexpr uint8_t MSG_REALLOC = 8;
constexpr uint8_t MSG_REALLOC_ACK = 9;

using FnCreateFromWindow = void* (*)(ANativeWindow*, const char*);
using FnReleaseControl = void (*)(void*);
using FnTransactionCreate = void* (*)();
using FnTransactionDelete = void (*)(void*);
using FnTransactionApply = void (*)(void*);
using FnSetBuffer = void (*)(void*, void*, AHardwareBuffer*, int);
using FnSetZOrder = void (*)(void*, void*, int32_t);
using FnSetVisibility = void (*)(void*, void*, int8_t);
using FnSetGeometry = void (*)(void*, void*, const ARect*, const ARect*, int32_t);
using FnSetBackPressure = void (*)(void*, void*, bool);
using FnSetCallback = void (*)(void*, void*, void (*)(void*, void*));
using FnSetFrameRate = void (*)(void*, void*, float, int8_t);

struct CallbackContext {
    int fd;
    uint32_t slot;
    uint8_t type;
    uint8_t displayed;
};

static void transactionCallback(void* opaque, void*) {
    auto* ctx = static_cast<CallbackContext*>(opaque);
    if (!ctx) return;

    DirectAHBCompositor::ReleaseMsg msg{};
    msg.type = ctx->type;
    msg.slotIndex = ctx->slot;
    msg.releaseFd = -1;
    msg.displayed = ctx->displayed;
    msg.vsyncTimeNs = 0;
    if (ctx->fd >= 0) {
        (void)send(ctx->fd, &msg, sizeof(msg), MSG_NOSIGNAL);
        close(ctx->fd);
    }
    delete ctx;
}
}

static_assert(sizeof(DirectAHBCompositor::PresentMsg) == 48, "AHB present protocol size mismatch");
static_assert(sizeof(DirectAHBCompositor::ReleaseMsg) == 24, "AHB release protocol size mismatch");

DirectAHBCompositor::DirectAHBCompositor() = default;

DirectAHBCompositor::~DirectAHBCompositor() {
    stop();
    std::lock_guard<std::mutex> lk(mutex);
    if (libAndroid) {
        dlclose(libAndroid);
        libAndroid = nullptr;
    }
}

bool DirectAHBCompositor::loadApiLocked() {
    if (fnCreateFromWindow) return true;
    if (android_get_device_api_level() < 29) {
        DLOGW("SurfaceControl DAC requires Android 10 / API 29+");
        return false;
    }

    libAndroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!libAndroid) {
        DLOGE("dlopen(libandroid.so) failed: %s", dlerror());
        return false;
    }

    fnCreateFromWindow   = dlsym(libAndroid, "ASurfaceControl_createFromWindow");
    fnReleaseControl     = dlsym(libAndroid, "ASurfaceControl_release");
    fnTransactionCreate  = dlsym(libAndroid, "ASurfaceTransaction_create");
    fnTransactionDelete  = dlsym(libAndroid, "ASurfaceTransaction_delete");
    fnTransactionApply   = dlsym(libAndroid, "ASurfaceTransaction_apply");
    fnSetBuffer          = dlsym(libAndroid, "ASurfaceTransaction_setBuffer");
    fnSetZOrder          = dlsym(libAndroid, "ASurfaceTransaction_setZOrder");
    fnSetVisibility      = dlsym(libAndroid, "ASurfaceTransaction_setVisibility");
    fnSetGeometry        = dlsym(libAndroid, "ASurfaceTransaction_setGeometry");
    fnSetBackPressure    = dlsym(libAndroid, "ASurfaceTransaction_setEnableBackPressure");
    fnSetOnComplete      = dlsym(libAndroid, "ASurfaceTransaction_setOnComplete");
    fnSetOnCommit        = dlsym(libAndroid, "ASurfaceTransaction_setOnCommit");
    fnSetFrameRate       = dlsym(libAndroid, "ASurfaceTransaction_setFrameRate");

    const bool ok = fnCreateFromWindow && fnReleaseControl &&
                    fnTransactionCreate && fnTransactionDelete && fnTransactionApply &&
                    fnSetBuffer && fnSetVisibility && fnSetGeometry;
    if (!ok) {
        DLOGE("required SurfaceControl symbols missing; DAC disabled");
        return false;
    }
    return true;
}

bool DirectAHBCompositor::createLayersLocked(ANativeWindow* parentWindow, float refreshRate) {
    if (!parentWindow || !loadApiLocked()) return false;
    destroyLayersLocked();

    auto create = reinterpret_cast<FnCreateFromWindow>(fnCreateFromWindow);
    gameControl = create(parentWindow, "winlator_dac_game");
    cursorControl = create(parentWindow, "winlator_dac_cursor");
    if (!gameControl || !cursorControl) {
        DLOGE("ASurfaceControl_createFromWindow failed");
        destroyLayersLocked();
        return false;
    }

    auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
    auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
    auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
    auto vis = reinterpret_cast<FnSetVisibility>(fnSetVisibility);
    void* t = tc();
    if (!t) {
        destroyLayersLocked();
        return false;
    }
    if (fnSetZOrder) {
        auto z = reinterpret_cast<FnSetZOrder>(fnSetZOrder);
        z(t, gameControl, 0);
        z(t, cursorControl, 1);
    }
    vis(t, gameControl, 0);
    vis(t, cursorControl, 0);
    if (fnSetFrameRate && refreshRate > 1.0f) {
        reinterpret_cast<FnSetFrameRate>(fnSetFrameRate)(t, gameControl, refreshRate, 0);
    }
    ta(t);
    td(t);
    DLOGI("SurfaceControl DAC layers created, refresh=%.2f", refreshRate);
    return true;
}

void DirectAHBCompositor::destroyLayersLocked() {
    auto rel = reinterpret_cast<FnReleaseControl>(fnReleaseControl);
    if (rel) {
        if (cursorControl) rel(cursorControl);
        if (gameControl) rel(gameControl);
    }
    cursorControl = nullptr;
    gameControl = nullptr;
    presenting.store(false, std::memory_order_release);
}

bool DirectAHBCompositor::start(ANativeWindow* parentWindow, int fd,
                                AHardwareBuffer* const* ahbs, int count,
                                int width, int height, float refreshRate) {
    stop();
    if (fd < 0 || !ahbs || count < 3 || count > 4) return false;

    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!createLayersLocked(parentWindow, refreshRate)) return false;
        socketFd = fd;
        logicalWidth = std::max(width, 1);
        logicalHeight = std::max(height, 1);
        buffers.assign(ahbs, ahbs + count);
        currentSlot = -1;
        paused = false;
        dstX = dstY = 0;
        dstW = logicalWidth;
        dstH = logicalHeight;
        running.store(true, std::memory_order_release);
    }

    receiverThread = std::thread(&DirectAHBCompositor::receiverLoop, this);
    return true;
}

bool DirectAHBCompositor::startLocal(ANativeWindow* parentWindow,
                                         int width, int height, float refreshRate) {
    stop();
    std::lock_guard<std::mutex> lk(mutex);
    if (!createLayersLocked(parentWindow, refreshRate)) return false;
    socketFd = -1;
    buffers.clear();
    logicalWidth = std::max(width, 1);
    logicalHeight = std::max(height, 1);
    currentSlot = -1;
    paused = false;
    dstX = dstY = 0;
    dstW = logicalWidth;
    dstH = logicalHeight;
    running.store(true, std::memory_order_release);
    DLOGI("local DRI3 direct-AHB mode started %dx%d", logicalWidth, logicalHeight);
    return true;
}

bool DirectAHBCompositor::submitExternalBuffer(AHardwareBuffer* ahb, int acquireFenceFd,
                                                int srcWidth, int srcHeight,
                                                int outX, int outY, int outW, int outH) {
    std::lock_guard<std::mutex> lk(mutex);
    if (!running.load(std::memory_order_acquire) || paused || !gameControl || !ahb) {
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        return false;
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    const int sw = srcWidth > 0 ? srcWidth : static_cast<int>(desc.width);
    const int sh = srcHeight > 0 ? srcHeight : static_cast<int>(desc.height);
    dstX = outX;
    dstY = outY;
    dstW = outW > 0 ? outW : logicalWidth;
    dstH = outH > 0 ? outH : logicalHeight;

    ARect src{0, 0, sw, sh};
    ARect dst{dstX, dstY, dstX + dstW, dstY + dstH};

    auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
    auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
    auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
    auto sb = reinterpret_cast<FnSetBuffer>(fnSetBuffer);
    auto sg = reinterpret_cast<FnSetGeometry>(fnSetGeometry);
    auto sv = reinterpret_cast<FnSetVisibility>(fnSetVisibility);
    void* t = tc();
    if (!t) {
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        return false;
    }

    // Ownership of acquireFenceFd is transferred to SurfaceFlinger.
    sb(t, gameControl, ahb, acquireFenceFd);
    sg(t, gameControl, &src, &dst, 0);
    sv(t, gameControl, 1);
    ta(t);
    td(t);

    presenting.store(true, std::memory_order_release);
    applyCursorLocked();
    return true;
}

void DirectAHBCompositor::hide() {
    std::lock_guard<std::mutex> lk(mutex);
    hideAndReleaseCurrentLocked();
}

void DirectAHBCompositor::stop() {
    running.store(false, std::memory_order_release);
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(mutex);
        fd = socketFd;
    }
    if (fd >= 0) shutdown(fd, SHUT_RD);
    if (receiverThread.joinable()) receiverThread.join();

    std::lock_guard<std::mutex> lk(mutex);
    if (currentSlot >= 0 && socketFd >= 0) {
        releaseSlot(static_cast<uint32_t>(currentSlot), false);
        currentSlot = -1;
    }
    if (socketFd >= 0) {
        close(socketFd);
        socketFd = -1;
    }
    destroyLayersLocked();
    if (cursorBuffer) {
        AHardwareBuffer_release(cursorBuffer);
        cursorBuffer = nullptr;
    }
    buffers.clear();
    paused = false;
}

void DirectAHBCompositor::releaseSlot(uint32_t slot, bool displayed) {
    if (socketFd < 0) return;
    ReleaseMsg rel{};
    rel.type = MSG_RELEASE;
    rel.slotIndex = slot;
    rel.releaseFd = -1;
    rel.displayed = displayed ? 1 : 0;
    rel.vsyncTimeNs = 0;
    (void)send(socketFd, &rel, sizeof(rel), MSG_NOSIGNAL);
}

void DirectAHBCompositor::sendTick() {
    if (socketFd < 0) return;
    ReleaseMsg tick{};
    tick.type = MSG_TICK;
    tick.releaseFd = -1;
    (void)send(socketFd, &tick, sizeof(tick), MSG_NOSIGNAL);
}

bool DirectAHBCompositor::receivePresent(PresentMsg& msg, int& receivedFenceFd) {
    receivedFenceFd = -1;
    char control[CMSG_SPACE(sizeof(int))] = {};
    iovec iov{&msg, sizeof(msg)};
    msghdr hdr{};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control;
    hdr.msg_controllen = sizeof(control);

    ssize_t got;
    do {
        got = recvmsg(socketFd, &hdr, MSG_WAITALL);
    } while (got < 0 && errno == EINTR && running.load());

    if (got <= 0 || static_cast<size_t>(got) != sizeof(msg)) return false;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr); cmsg; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&receivedFenceFd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }
    return true;
}

bool DirectAHBCompositor::submitFrameLocked(const PresentMsg& msg, int acquireFenceFd) {
    if (paused || !gameControl || msg.slotIndex >= buffers.size() || !buffers[msg.slotIndex]) {
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        releaseSlot(msg.slotIndex, false);
        return false;
    }
    // Direct-render mode may expose BGRA bytes in an RGBA AHB. The first
    // implementation intentionally uses the layer's normalized blit mode.
    if (msg.bgraBytes != 0) {
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        DLOGW("BGRA direct-render frame rejected; falling back to normalized AHB mode");
        releaseSlot(msg.slotIndex, false);
        return false;
    }

    AHardwareBuffer* ahb = buffers[msg.slotIndex];
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    ARect src{0, 0, static_cast<int32_t>(desc.width), static_cast<int32_t>(desc.height)};

    dstX = msg.dstX;
    dstY = msg.dstY;
    dstW = msg.dstW > 0 ? msg.dstW : logicalWidth;
    dstH = msg.dstH > 0 ? msg.dstH : logicalHeight;
    ARect dst{dstX, dstY, dstX + dstW, dstY + dstH};

    auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
    auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
    auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
    auto sb = reinterpret_cast<FnSetBuffer>(fnSetBuffer);
    auto sg = reinterpret_cast<FnSetGeometry>(fnSetGeometry);
    auto sv = reinterpret_cast<FnSetVisibility>(fnSetVisibility);
    void* t = tc();
    if (!t) {
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        releaseSlot(msg.slotIndex, false);
        return false;
    }

    sb(t, gameControl, ahb, acquireFenceFd); // SurfaceFlinger owns the acquire fence.
    sg(t, gameControl, &src, &dst, 0);
    sv(t, gameControl, 1);
    if (fnSetBackPressure)
        reinterpret_cast<FnSetBackPressure>(fnSetBackPressure)(t, gameControl, false);

    const int oldSlot = currentSlot;
    if (fnSetOnCommit && socketFd >= 0) {
        int callbackFd = dup(socketFd);
        if (callbackFd >= 0) {
            auto* ctx = new CallbackContext{callbackFd, msg.slotIndex, MSG_TICK, 0};
            reinterpret_cast<FnSetCallback>(fnSetOnCommit)(t, ctx, transactionCallback);
        }
    }
    if (oldSlot >= 0 && socketFd >= 0) {
        if (fnSetOnComplete) {
            int callbackFd = dup(socketFd);
            if (callbackFd >= 0) {
                const uint8_t displayed = fnSetOnCommit ? 0 : 1;
                auto* ctx = new CallbackContext{callbackFd, static_cast<uint32_t>(oldSlot),
                                                MSG_RELEASE, displayed};
                reinterpret_cast<FnSetCallback>(fnSetOnComplete)(t, ctx, transactionCallback);
            } else {
                releaseSlot(static_cast<uint32_t>(oldSlot), false);
            }
        } else {
            releaseSlot(static_cast<uint32_t>(oldSlot), true);
        }
    }

    currentSlot = static_cast<int>(msg.slotIndex);
    ta(t);
    td(t);
    presenting.store(true, std::memory_order_release);
    applyCursorLocked();
    return true;
}

void DirectAHBCompositor::receiverLoop() {
    DLOGI("AHB receiver started fd=%d buffers=%zu", socketFd, buffers.size());
    while (running.load(std::memory_order_acquire)) {
        PresentMsg msg{};
        int fenceFd = -1;
        if (!receivePresent(msg, fenceFd)) break;

        std::lock_guard<std::mutex> lk(mutex);
        if (!running.load()) {
            if (fenceFd >= 0) close(fenceFd);
            break;
        }
        if (msg.type == MSG_REALLOC) {
            if (fenceFd >= 0) close(fenceFd);
            ReleaseMsg ack{};
            ack.type = MSG_REALLOC_ACK;
            ack.slotIndex = 0; // unsupported geometry: guest layer falls back safely.
            ack.releaseFd = -1;
            (void)send(socketFd, &ack, sizeof(ack), MSG_NOSIGNAL);
            continue;
        }
        if (msg.type != MSG_PRESENT) {
            if (fenceFd >= 0) close(fenceFd);
            continue;
        }
        submitFrameLocked(msg, fenceFd);
    }
    presenting.store(false, std::memory_order_release);
    DLOGI("AHB receiver stopped");
}

void DirectAHBCompositor::hideAndReleaseCurrentLocked() {
    if (gameControl && fnTransactionCreate) {
        auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
        auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
        auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
        auto sv = reinterpret_cast<FnSetVisibility>(fnSetVisibility);
        void* t = tc();
        if (t) {
            sv(t, gameControl, 0);
            if (cursorControl) sv(t, cursorControl, 0);
            if (currentSlot >= 0 && fnSetOnComplete && socketFd >= 0) {
                int callbackFd = dup(socketFd);
                if (callbackFd >= 0) {
                    auto* ctx = new CallbackContext{callbackFd, static_cast<uint32_t>(currentSlot),
                                                    MSG_RELEASE, 0};
                    reinterpret_cast<FnSetCallback>(fnSetOnComplete)(t, ctx, transactionCallback);
                }
                currentSlot = -1;
            }
            ta(t);
            td(t);
        }
    }
    if (currentSlot >= 0) {
        releaseSlot(static_cast<uint32_t>(currentSlot), false);
        currentSlot = -1;
    }
    presenting.store(false, std::memory_order_release);
}

void DirectAHBCompositor::detachSurface() {
    std::lock_guard<std::mutex> lk(mutex);
    paused = true;
    hideAndReleaseCurrentLocked();
    destroyLayersLocked();
}

bool DirectAHBCompositor::reattachSurface(ANativeWindow* parentWindow, float refreshRate) {
    std::lock_guard<std::mutex> lk(mutex);
    if (!running.load()) return false;
    const bool ok = createLayersLocked(parentWindow, refreshRate);
    paused = !ok;
    if (ok) applyCursorLocked();
    return ok;
}

void DirectAHBCompositor::updatePointerPosition(short x, short y) {
    std::lock_guard<std::mutex> lk(mutex);
    pointerX = x;
    pointerY = y;
    if (presenting.load()) applyCursorLocked();
}

void DirectAHBCompositor::setCursorVisible(bool visible) {
    std::lock_guard<std::mutex> lk(mutex);
    cursorVisible = visible;
    applyCursorLocked();
}

void DirectAHBCompositor::updateCursorImage(const void* pixels, short w, short h,
                                            short hotX, short hotY) {
    if (!pixels || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(mutex);

    if (cursorBuffer && (cursorW != w || cursorH != h)) {
        AHardwareBuffer_release(cursorBuffer);
        cursorBuffer = nullptr;
    }
    if (!cursorBuffer) {
        AHardwareBuffer_Desc desc{};
        desc.width = static_cast<uint32_t>(w);
        desc.height = static_cast<uint32_t>(h);
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        if (AHardwareBuffer_allocate(&desc, &cursorBuffer) != 0) {
            cursorBuffer = nullptr;
            return;
        }
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(cursorBuffer, &desc);
    void* dst = nullptr;
    if (AHardwareBuffer_lock(cursorBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                             -1, nullptr, &dst) != 0 || !dst) return;
    const auto* src = static_cast<const uint32_t*>(pixels);
    auto* out = static_cast<uint32_t*>(dst);
    for (int row = 0; row < h; ++row) {
        memcpy(out + static_cast<size_t>(row) * desc.stride,
               src + static_cast<size_t>(row) * w,
               static_cast<size_t>(w) * 4);
    }
    int fenceFd = -1;
    AHardwareBuffer_unlock(cursorBuffer, &fenceFd);
    cursorW = w;
    cursorH = h;
    cursorHotX = hotX;
    cursorHotY = hotY;

    if (cursorControl && fnTransactionCreate) {
        auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
        auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
        auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
        void* t = tc();
        if (t) {
            reinterpret_cast<FnSetBuffer>(fnSetBuffer)(t, cursorControl, cursorBuffer, fenceFd);
            reinterpret_cast<FnSetVisibility>(fnSetVisibility)(t, cursorControl,
                (cursorVisible && presenting.load()) ? 1 : 0);
            ta(t);
            td(t);
            fenceFd = -1; // ownership transferred
        }
    }
    if (fenceFd >= 0) close(fenceFd);
    applyCursorLocked();
}

void DirectAHBCompositor::applyCursorLocked() {
    if (!cursorControl || !cursorBuffer || !fnTransactionCreate) return;
    auto tc = reinterpret_cast<FnTransactionCreate>(fnTransactionCreate);
    auto td = reinterpret_cast<FnTransactionDelete>(fnTransactionDelete);
    auto ta = reinterpret_cast<FnTransactionApply>(fnTransactionApply);
    auto sg = reinterpret_cast<FnSetGeometry>(fnSetGeometry);
    auto sv = reinterpret_cast<FnSetVisibility>(fnSetVisibility);
    void* t = tc();
    if (!t) return;

    const float sx = logicalWidth > 0 ? static_cast<float>(dstW) / logicalWidth : 1.0f;
    const float sy = logicalHeight > 0 ? static_cast<float>(dstH) / logicalHeight : 1.0f;
    const int left = dstX + static_cast<int>((pointerX - cursorHotX) * sx);
    const int top = dstY + static_cast<int>((pointerY - cursorHotY) * sy);
    const int outW = std::max(1, static_cast<int>(cursorW * sx));
    const int outH = std::max(1, static_cast<int>(cursorH * sy));
    ARect src{0, 0, cursorW, cursorH};
    ARect dst{left, top, left + outW, top + outH};
    sg(t, cursorControl, &src, &dst, 0);
    sv(t, cursorControl, (cursorVisible && presenting.load() && !paused) ? 1 : 0);
    ta(t);
    td(t);
}
