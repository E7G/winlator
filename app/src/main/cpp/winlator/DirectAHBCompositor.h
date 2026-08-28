#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

/**
 * Direct AHardwareBuffer -> SurfaceFlinger presenter for the 3.1 renderer.
 *
 * It is deliberately independent of VulkanRendererContext's swapchain. The
 * existing Vulkan renderer remains alive as a fallback; once a valid direct
 * frame is committed this child SurfaceControl covers the normal renderer.
 */
class DirectAHBCompositor {
public:
    DirectAHBCompositor();
    ~DirectAHBCompositor();

    bool start(ANativeWindow* parentWindow, int socketFd,
               AHardwareBuffer* const* buffers, int bufferCount,
               int logicalWidth, int logicalHeight, float refreshRate);
    bool startLocal(ANativeWindow* parentWindow, int logicalWidth, int logicalHeight,
                    float refreshRate);
    bool submitExternalBuffer(AHardwareBuffer* ahb, int acquireFenceFd,
                              int srcWidth, int srcHeight,
                              int dstX, int dstY, int dstWidth, int dstHeight);
    void hide();
    void stop();

    void detachSurface();
    bool reattachSurface(ANativeWindow* parentWindow, float refreshRate);

    void updatePointerPosition(short x, short y);
    void updateCursorImage(const void* pixels, short w, short h, short hotX, short hotY);
    void setCursorVisible(bool visible);

    bool isPresenting() const { return presenting.load(std::memory_order_acquire); }
    bool isRunning() const { return running.load(std::memory_order_acquire); }

    // Wire layout shared with the Plus AHB Vulkan layer. Keep ABI stable.
    struct PresentMsg {
        uint8_t type;
        uint32_t slotIndex;
        int32_t acquireFd;
        int32_t dstX, dstY, dstW, dstH;
        uint64_t presentId;
        uint8_t bgraBytes;
    };
    struct ReleaseMsg {
        uint8_t type;
        uint32_t slotIndex;
        int32_t releaseFd;
        uint8_t displayed;
        uint64_t vsyncTimeNs;
    };

private:
    bool loadApiLocked();
    bool createLayersLocked(ANativeWindow* parentWindow, float refreshRate);
    void destroyLayersLocked();
    void hideAndReleaseCurrentLocked();
    bool submitFrameLocked(const PresentMsg& msg, int acquireFenceFd);
    void releaseSlot(uint32_t slot, bool displayed);
    void sendTick();
    void receiverLoop();
    bool receivePresent(PresentMsg& msg, int& receivedFenceFd);
    void applyCursorLocked();
    void releaseBuffersLocked();
    bool reallocateBuffersLocked(int width, int height, int count);
    bool sendReallocAckAndBuffersLocked(const std::vector<AHardwareBuffer*>& newBuffers);

    std::mutex mutex;
    std::shared_ptr<std::mutex> socketWriteMutex = std::make_shared<std::mutex>();
    std::atomic<bool> running{false};
    std::atomic<bool> presenting{false};
    std::thread receiverThread;

    int socketFd = -1;
    std::vector<AHardwareBuffer*> buffers;
    int logicalWidth = 0;
    int logicalHeight = 0;
    int currentSlot = -1;
    bool paused = false;

    void* libAndroid = nullptr;
    void* gameControl = nullptr;
    void* cursorControl = nullptr;
    void* fnCreateFromWindow = nullptr;
    void* fnReleaseControl = nullptr;
    void* fnTransactionCreate = nullptr;
    void* fnTransactionDelete = nullptr;
    void* fnTransactionApply = nullptr;
    void* fnSetBuffer = nullptr;
    void* fnSetZOrder = nullptr;
    void* fnSetVisibility = nullptr;
    void* fnSetGeometry = nullptr;
    void* fnSetBackPressure = nullptr;
    void* fnSetOnComplete = nullptr;
    void* fnSetOnCommit = nullptr;
    void* fnSetFrameRate = nullptr;

    int dstX = 0, dstY = 0, dstW = 0, dstH = 0;

    AHardwareBuffer* cursorBuffer = nullptr;
    short cursorW = 0, cursorH = 0, cursorHotX = 0, cursorHotY = 0;
    short pointerX = 0, pointerY = 0;
    bool cursorVisible = false;
};
