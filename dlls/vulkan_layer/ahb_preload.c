/*
 * AHB Direct Compositing - LD_PRELOAD Interceptor
 *
 * This .so is loaded via LD_PRELOAD before any other library. It overrides
 * vkCreateSwapchainKHR, vkAcquireNextImageKHR, vkGetSwapchainImagesKHR,
 * and vkQueuePresentKHR at the dynamic linker level using RTLD_NEXT.
 *
 * When ANDROID_AHB_SERVER is set, it intercepts swapchain operations and
 * routes frames through AHardwareBuffers to SurfaceFlinger.
 * When not set, it's a zero-overhead passthrough (just dlsym + forward).
 *
 * Copyright 2024 Winlator contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "../wineandroid.drv/vulkan_ahb.h"

#define LOG_TAG "AHB_Preload"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Real function pointers (resolved via RTLD_NEXT or dlsym) */
static PFN_vkCreateSwapchainKHR    real_CreateSwapchainKHR = NULL;
static PFN_vkDestroySwapchainKHR   real_DestroySwapchainKHR = NULL;
static PFN_vkGetSwapchainImagesKHR real_GetSwapchainImagesKHR = NULL;
static PFN_vkAcquireNextImageKHR   real_AcquireNextImageKHR = NULL;
static PFN_vkQueuePresentKHR       real_QueuePresentKHR = NULL;
static PFN_vkGetDeviceProcAddr     real_GetDeviceProcAddr = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties real_GetPhysMemProps = NULL;

/* AHB state */
static int g_ahb_socket_fd = -1;
static int g_ahb_active = 0;
static int g_ahb_initialized = 0;
static struct wine_vk_swapchain *g_ahb_swapchain = NULL;
static VkDevice g_device = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys_device = VK_NULL_HANDLE;

/* Pre-received AHB handles (sent by Android on connection) */
static AHardwareBuffer *g_ahb_buffers[3] = {NULL, NULL, NULL};
static int g_ahb_buffer_count = 0;

/* ========================================================================
 * Initialization (called once on first Vulkan call)
 * ======================================================================== */

static void init_ahb(void)
{
    if (g_ahb_initialized) return;
    g_ahb_initialized = 1;

    const char *server_path = getenv("ANDROID_AHB_SERVER");
    if (!server_path || server_path[0] == '\0') {
        LOGI("init: ANDROID_AHB_SERVER not set, passthrough mode");
        return;
    }

    /* Don't connect yet — wait until vkCreateDevice is called.
     * This prevents Wine helper processes (wineserver, services.exe, etc.)
     * from connecting to the socket and disrupting the present receiver. */
    g_ahb_active = 0;
    LOGI("init: ANDROID_AHB_SERVER=%s, will connect on first vkCreateDevice", server_path);
}

/* Actually connect to the AHB server and receive buffers.
 * Called from vkCreateDevice only when a real Vulkan device is being created. */
static void connect_ahb(void)
{
    if (g_ahb_socket_fd >= 0) return; /* Already connected */

    const char *server_path = getenv("ANDROID_AHB_SERVER");
    if (!server_path || server_path[0] == '\0') return;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("connect: socket() failed: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, server_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("connect: connect(%s) failed: %s", server_path, strerror(errno));
        close(fd);
        return;
    }

    g_ahb_socket_fd = fd;
    g_ahb_active = 1;
    LOGI("connect: Connected to AHB server at %s (fd=%d) — DIRECT PATH ACTIVE", server_path, fd);

    /* Receive the 3 pre-sent AHardwareBuffer handles from the Android app. */
    for (int i = 0; i < 3; i++) {
        AHardwareBuffer *ahb = NULL;
        int recv_result = AHardwareBuffer_recvHandleFromUnixSocket(fd, &ahb);
        if (recv_result != 0 || !ahb) {
            LOGE("connect: Failed to receive AHB handle %d (result=%d)", i, recv_result);
            g_ahb_active = 0;
            close(fd);
            g_ahb_socket_fd = -1;
            return;
        }
        g_ahb_buffers[i] = ahb;
        g_ahb_buffer_count++;
        LOGI("connect: Received AHB handle %d", i);
    }
    LOGI("connect: All %d AHB handles received, ready for swapchain creation", g_ahb_buffer_count);
}

/* ========================================================================
 * Resolve real functions via RTLD_NEXT
 * ======================================================================== */

static void resolve_real_functions(void)
{
    if (real_CreateSwapchainKHR) return;

    real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)dlsym(RTLD_NEXT, "vkCreateSwapchainKHR");
    real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)dlsym(RTLD_NEXT, "vkDestroySwapchainKHR");
    real_GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)dlsym(RTLD_NEXT, "vkGetSwapchainImagesKHR");
    real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)dlsym(RTLD_NEXT, "vkAcquireNextImageKHR");
    real_QueuePresentKHR = (PFN_vkQueuePresentKHR)dlsym(RTLD_NEXT, "vkQueuePresentKHR");
    real_GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    real_GetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(RTLD_NEXT, "vkGetPhysicalDeviceMemoryProperties");

    if (!real_CreateSwapchainKHR) {
        LOGW("resolve: vkCreateSwapchainKHR not found via RTLD_NEXT");
    }
}

/* ========================================================================
 * Intercepted Vulkan functions (exported, override via LD_PRELOAD)
 * ======================================================================== */

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    init_ahb();

    /* Connect to AHB server on first vkCreateDevice call.
     * This ensures only the actual Vulkan-using process connects,
     * not Wine helper processes like wineserver or services.exe. */
    connect_ahb();
    resolve_real_functions();

    typedef VkResult (*PFN)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
    PFN real_fn = (PFN)dlsym(RTLD_NEXT, "vkCreateDevice");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS) {
        g_device = *pDevice;
        g_phys_device = physicalDevice;

        /* Resolve real device-level functions through the REAL GetDeviceProcAddr
         * so our wrappers can forward non-AHB calls correctly. */
        if (real_GetDeviceProcAddr) {
            if (!real_CreateSwapchainKHR)
                real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)
                    real_GetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
            if (!real_DestroySwapchainKHR)
                real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)
                    real_GetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
            if (!real_GetSwapchainImagesKHR)
                real_GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)
                    real_GetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
            if (!real_AcquireNextImageKHR)
                real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)
                    real_GetDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR");
            if (!real_QueuePresentKHR)
                real_QueuePresentKHR = (PFN_vkQueuePresentKHR)
                    real_GetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
            if (!real_GetPhysMemProps)
                real_GetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
                    dlsym(RTLD_NEXT, "vkGetPhysicalDeviceMemoryProperties");
        }

        /* Patch the device dispatch table so that even when DXVK calls
         * vkGetDeviceProcAddr through the device's own vtable (bypassing
         * LD_PRELOAD), it still hits our wrapper.
         *
         * The Vulkan ICD dispatch table starts at the first pointer in the
         * VkDevice struct. We scan it to find the real vkGetDeviceProcAddr
         * entry and replace it with ours. */
        if (g_ahb_active) {
            void **dispatch = *(void ***)(*pDevice);
            if (dispatch) {
                /* Find vkGetDeviceProcAddr in the dispatch table by comparing
                 * function pointers. Scan first 32 entries (safe range). */
                PFN_vkGetDeviceProcAddr real_gdpa = real_GetDeviceProcAddr;
                int patched = 0;
                for (int i = 0; i < 32 && !patched; i++) {
                    if (dispatch[i] == (void*)real_gdpa) {
                        dispatch[i] = (void*)vkGetDeviceProcAddr;
                        patched = 1;
                        LOGI("vkCreateDevice: patched dispatch table at index %d", i);
                    }
                }
                if (!patched) {
                    LOGW("vkCreateDevice: could not find vkGetDeviceProcAddr in dispatch table");
                }
            }
        }

        LOGI("vkCreateDevice intercepted: device=%p, AHB active=%d", (void*)*pDevice, g_ahb_active);
    }
    return res;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    init_ahb();
    resolve_real_functions();

    if (!g_ahb_active || !real_CreateSwapchainKHR) {
        if (real_CreateSwapchainKHR)
            return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (g_ahb_buffer_count == 0) {
        LOGW("vkCreateSwapchainKHR: No AHB buffers available, falling back");
        g_ahb_active = 0;
        return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    /* Only intercept the main game swapchain (reasonable dimensions) */
    if (pCreateInfo->imageExtent.width < 64 || pCreateInfo->imageExtent.height < 64) {
        LOGI("vkCreateSwapchainKHR: Small swapchain (%dx%d), passthrough",
             pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
        return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    /* Create AHB swapchain using pre-received buffers.
     * We construct a wine_vk_surface that already has the buffers,
     * then call wine_ahb_create_swapchain with a modified approach:
     * skip the MSG_REQUEST/recv and use our pre-received AHBs directly. */

    /* Store the pre-received buffers back into the socket so
     * wine_ahb_create_swapchain can recv them normally.
     * Actually — simpler: just call import_ahb_to_vk_image directly. */

    /* Allocate swapchain struct manually */
    struct wine_vk_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        LOGE("vkCreateSwapchainKHR: allocation failed");
        return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    sc->device = device;
    sc->image_count = (g_ahb_buffer_count < 3) ? g_ahb_buffer_count : 3;
    sc->current_image = 0;

    /* Load device function pointers */
    sc->pfn_vkGetFenceFdKHR = (PFN_vkGetFenceFdKHR)
        real_GetDeviceProcAddr(device, "vkGetFenceFdKHR");
    sc->pfn_vkImportFenceFdKHR = (PFN_vkImportFenceFdKHR)
        real_GetDeviceProcAddr(device, "vkImportFenceFdKHR");
    sc->pfn_vkCreateFence = (PFN_vkCreateFence)
        real_GetDeviceProcAddr(device, "vkCreateFence");
    sc->pfn_vkDestroyFence = (PFN_vkDestroyFence)
        real_GetDeviceProcAddr(device, "vkDestroyFence");
    sc->pfn_vkWaitForFences = (PFN_vkWaitForFences)
        real_GetDeviceProcAddr(device, "vkWaitForFences");
    sc->pfn_vkResetFences = (PFN_vkResetFences)
        real_GetDeviceProcAddr(device, "vkResetFences");
    sc->pfn_vkAllocateMemory = (PFN_vkAllocateMemory)
        real_GetDeviceProcAddr(device, "vkAllocateMemory");
    sc->pfn_vkFreeMemory = (PFN_vkFreeMemory)
        real_GetDeviceProcAddr(device, "vkFreeMemory");
    sc->pfn_vkCreateImage = (PFN_vkCreateImage)
        real_GetDeviceProcAddr(device, "vkCreateImage");
    sc->pfn_vkDestroyImage = (PFN_vkDestroyImage)
        real_GetDeviceProcAddr(device, "vkDestroyImage");
    sc->pfn_vkBindImageMemory2 = (PFN_vkBindImageMemory2)
        real_GetDeviceProcAddr(device, "vkBindImageMemory2");
    sc->pfn_vkGetDeviceProcAddr = real_GetDeviceProcAddr;

    /* Create a fake surface for the swapchain */
    static struct wine_vk_surface fake_surface;
    fake_surface.socket_fd = g_ahb_socket_fd;
    fake_surface.width = (int)pCreateInfo->imageExtent.width;
    fake_surface.height = (int)pCreateInfo->imageExtent.height;
    fake_surface.ahb_supported = true;
    sc->surface = &fake_surface;

    /* Create reuse fences */
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = NULL,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult fres = sc->pfn_vkCreateFence(device, &fence_info, NULL,
                                               &sc->images[i].reuse_fence);
        if (fres != VK_SUCCESS) {
            LOGE("vkCreateSwapchainKHR: vkCreateFence failed for slot %u", i);
            free(sc);
            return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        sc->images[i].release_fence = -1;
        sc->images[i].in_use = false;
    }

    /* Import each pre-received AHB into Vulkan */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult ires = import_ahb_to_vk_image(sc, i, g_ahb_buffers[i],
                                                g_phys_device, real_GetPhysMemProps);
        if (ires != VK_SUCCESS) {
            LOGE("vkCreateSwapchainKHR: import_ahb_to_vk_image failed for slot %u: %d", i, ires);
            /* Cleanup and fall back */
            for (uint32_t j = 0; j < i; j++) {
                sc->pfn_vkDestroyImage(device, sc->images[j].vk_image, NULL);
                sc->pfn_vkFreeMemory(device, sc->images[j].vk_memory, NULL);
            }
            for (uint32_t j = 0; j < sc->image_count; j++) {
                sc->pfn_vkDestroyFence(device, sc->images[j].reuse_fence, NULL);
            }
            free(sc);
            LOGW("vkCreateSwapchainKHR: AHB import failed, falling back to real driver");
            return real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        LOGI("vkCreateSwapchainKHR: slot %u import OK (image=%p)", i, (void*)sc->images[i].vk_image);
    }

    LOGI("vkCreateSwapchainKHR: all imports done, finalizing swapchain");
    LOGI("vkCreateSwapchainKHR: sc=%p, pSwapchain=%p", (void*)sc, (void*)pSwapchain);
    g_ahb_swapchain = sc;
    *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    LOGI("vkCreateSwapchainKHR: AHB SWAPCHAIN CREATED! (%dx%d, %u images) — DIRECT COMPOSITING ACTIVE",
         pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, sc->image_count);
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    if (g_ahb_active && g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        VkResult r = wine_ahb_acquire_next_image(g_ahb_swapchain, timeout, semaphore, fence, pImageIndex);
        static int _acqCount = 0;
        if (++_acqCount <= 5 || (_acqCount % 120 == 0)) {
            LOGI("vkAcquireNextImageKHR: frame %d, result=%d, imageIndex=%u",
                 _acqCount, r, pImageIndex ? *pImageIndex : 999);
        }
        return r;
    }
    resolve_real_functions();
    if (real_AcquireNextImageKHR)
        return real_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    return VK_ERROR_DEVICE_LOST;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pCount, VkImage *pImages)
{
    if (g_ahb_active && g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        VkResult r = wine_ahb_get_swapchain_images(g_ahb_swapchain, pCount, pImages);
        LOGI("vkGetSwapchainImagesKHR: count=%u, images=%p, result=%d",
             pCount ? *pCount : 0, (void*)pImages, r);
        return r;
    }
    resolve_real_functions();
    if (real_GetSwapchainImagesKHR)
        return real_GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    return VK_ERROR_DEVICE_LOST;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    /* Check if any of the swapchains in this present call is ours */
    if (g_ahb_active && g_ahb_swapchain) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            if ((VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == pPresentInfo->pSwapchains[i]) {
                /* This is our AHB swapchain — present through our path */
                VkResult r = wine_ahb_queue_present(
                    g_ahb_swapchain, queue, pPresentInfo->pImageIndices[i], VK_NULL_HANDLE);
                static int _presentCount = 0;
                if (++_presentCount <= 5 || (_presentCount % 60 == 0)) {
                    LOGI("vkQueuePresentKHR: frame %d presented (slot=%u, result=%d)",
                         _presentCount, pPresentInfo->pImageIndices[i], r);
                }
                if (pPresentInfo->pResults) pPresentInfo->pResults[i] = r;
                return r;
            }
        }
    }
    resolve_real_functions();
    if (real_QueuePresentKHR)
        return real_QueuePresentKHR(queue, pPresentInfo);
    return VK_ERROR_DEVICE_LOST;
}

__attribute__((visibility("default")))
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    if (g_ahb_active && g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        LOGI("vkDestroySwapchainKHR: destroying AHB swapchain");
        wine_ahb_destroy_swapchain(g_ahb_swapchain);
        g_ahb_swapchain = NULL;
        return;
    }
    resolve_real_functions();
    if (real_DestroySwapchainKHR)
        real_DestroySwapchainKHR(device, swapchain, pAllocator);
}

/* ========================================================================
 * The critical intercept: vkGetDeviceProcAddr
 *
 * DXVK calls this to get function pointers for swapchain operations.
 * We return our wrapped versions so frames go through the AHB path.
 * ======================================================================== */

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char *pName)
{
    init_ahb();
    resolve_real_functions();

    /* Only intercept when AHB path is active (socket connected).
     * Our wrappers internally check the swapchain handle to decide
     * whether to use AHB path or forward to real driver. */
    if (g_ahb_active) {
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            LOGI("vkGetDeviceProcAddr: returning OUR vkCreateSwapchainKHR");
            return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
        }
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
            return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
            LOGI("vkGetDeviceProcAddr: returning OUR vkGetSwapchainImagesKHR");
            return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
        }
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
            LOGI("vkGetDeviceProcAddr: returning OUR vkAcquireNextImageKHR");
            return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            LOGI("vkGetDeviceProcAddr: returning OUR vkQueuePresentKHR");
            return (PFN_vkVoidFunction)vkQueuePresentKHR;
        }
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
            return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }

    /* Forward everything else to the real driver */
    typedef PFN_vkVoidFunction (*PFN_GetDeviceProcAddr)(VkDevice, const char*);
    PFN_GetDeviceProcAddr real_fn = (PFN_GetDeviceProcAddr)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    if (real_fn) return real_fn(device, pName);
    return NULL;
}

/* Also intercept vkGetInstanceProcAddr for completeness */
__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char *pName)
{
    init_ahb();

    if (g_ahb_active) {
        if (strcmp(pName, "vkCreateDevice") == 0)
            return (PFN_vkVoidFunction)vkCreateDevice;
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
            return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
            return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
            return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
            return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
        if (strcmp(pName, "vkQueuePresentKHR") == 0)
            return (PFN_vkVoidFunction)vkQueuePresentKHR;
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
            return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
        if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
            return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    }

    typedef PFN_vkVoidFunction (*PFN_GetInstanceProcAddr)(VkInstance, const char*);
    PFN_GetInstanceProcAddr real_fn = (PFN_GetInstanceProcAddr)dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    if (real_fn) return real_fn(instance, pName);
    return NULL;
}
