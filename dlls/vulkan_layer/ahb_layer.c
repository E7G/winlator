/*
 * AHB Direct Compositing - Vulkan Implicit Layer
 *
 * This is a proper Vulkan layer that intercepts swapchain operations to
 * redirect rendering through AHardwareBuffers for direct compositing.
 *
 * Unlike the ICD wrapper approach, a layer is guaranteed to be invoked
 * by the Vulkan loader regardless of Box64 thunk routing.
 *
 * Copyright 2024 Winlator contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "../wineandroid.drv/vulkan_ahb.h"

#define LOG_TAG "AHB_LAYER"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ========================================================================
 * Per-instance and per-device dispatch tables
 * ======================================================================== */

typedef struct {
    PFN_vkGetInstanceProcAddr         GetInstanceProcAddr;
    PFN_vkDestroyInstance             DestroyInstance;
    PFN_vkEnumeratePhysicalDevices    EnumeratePhysicalDevices;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice                CreateDevice;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
} InstanceDispatch;

typedef struct {
    PFN_vkGetDeviceProcAddr       GetDeviceProcAddr;
    PFN_vkDestroyDevice           DestroyDevice;
    PFN_vkCreateSwapchainKHR      CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR     DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR   GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR     AcquireNextImageKHR;
    PFN_vkQueuePresentKHR         QueuePresentKHR;
    PFN_vkQueueSubmit             QueueSubmit;
    PFN_vkCreateCommandPool       CreateCommandPool;
    PFN_vkAllocateCommandBuffers  AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer      BeginCommandBuffer;
    PFN_vkEndCommandBuffer        EndCommandBuffer;
    PFN_vkCmdCopyImage            CmdCopyImage;
    PFN_vkCmdPipelineBarrier      CmdPipelineBarrier;
    PFN_vkCreateFence             CreateFence;
    PFN_vkDestroyFence            DestroyFence;
    PFN_vkWaitForFences           WaitForFences;
    PFN_vkResetFences             ResetFences;
    PFN_vkResetCommandBuffer      ResetCommandBuffer;
    PFN_vkDestroyCommandPool      DestroyCommandPool;
    PFN_vkQueueWaitIdle           QueueWaitIdle;
} DeviceDispatch;

/* Simple single-instance/device support (sufficient for Wine/Box64) */
static InstanceDispatch g_inst_dispatch;
static DeviceDispatch   g_dev_dispatch;
static VkInstance       g_instance    = VK_NULL_HANDLE;
static VkDevice         g_device      = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys_device = VK_NULL_HANDLE;

/* AHB state */
static int              g_ahb_socket_fd    = -1;
static int              g_ahb_active       = 0;
static int              g_ahb_connected    = 0;
static AHardwareBuffer *g_ahb_buffers[AHB_MAX_IMAGES] = {NULL, NULL, NULL, NULL};
static int              g_ahb_buffer_count = 0;
static struct wine_vk_swapchain *g_ahb_swapchain = NULL;
static VkSwapchainKHR            g_real_swapchain_handle = VK_NULL_HANDLE;
static VkImage                   g_trojan_images[AHB_MAX_IMAGES] = {0};
static uint32_t                  g_trojan_image_count = 0;
static VkCommandPool             g_copy_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer           g_copy_cmd_buf = VK_NULL_HANDLE;
static VkFence                   g_copy_fence = VK_NULL_HANDLE;

/* ========================================================================
 * Layer chaining helper
 * ======================================================================== */

static inline VkLayerInstanceCreateInfo *get_instance_chain_info(
    const VkInstanceCreateInfo *pCreateInfo, VkLayerFunction func)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            chain->function == func)
            return chain;
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    }
    return NULL;
}

static inline VkLayerDeviceCreateInfo *get_device_chain_info(
    const VkDeviceCreateInfo *pCreateInfo, VkLayerFunction func)
{
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            chain->function == func)
            return chain;
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    }
    return NULL;
}

/* ========================================================================
 * AHB socket connection
 * ======================================================================== */

static void connect_ahb(void)
{
    if (g_ahb_connected) return;
    g_ahb_connected = 1;

    const char *server_path = getenv("ANDROID_AHB_SERVER");
    if (!server_path || server_path[0] == '\0') {
        LOGI("connect_ahb: ANDROID_AHB_SERVER not set, passthrough mode");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { LOGE("connect_ahb: socket() failed: %s", strerror(errno)); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, server_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("connect_ahb: connect(%s) failed: %s", server_path, strerror(errno));
        close(fd);
        return;
    }

    g_ahb_socket_fd = fd;
    LOGI("connect_ahb: connected to %s (fd=%d)", server_path, fd);

    for (int i = 0; i < AHB_MAX_IMAGES; i++) {
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_recvHandleFromUnixSocket(fd, &ahb) != 0 || !ahb) {
            LOGE("connect_ahb: failed to receive AHB %d", i);
            close(fd);
            g_ahb_socket_fd = -1;
            return;
        }
        g_ahb_buffers[i] = ahb;
        g_ahb_buffer_count++;
        LOGI("connect_ahb: received AHB handle %d", i);
    }

    g_ahb_active = 1;
    LOGI("connect_ahb: DIRECT PATH ACTIVE — %d buffers ready", g_ahb_buffer_count);
}

/* ========================================================================
 * Intercepted instance-level functions (surface queries)
 * ======================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
    if (!g_ahb_active) {
        return g_inst_dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    }

    /* Return capabilities that exactly match our AHB swapchain setup */
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(g_ahb_buffers[0], &desc);

    pSurfaceCapabilities->minImageCount = g_ahb_buffer_count;
    pSurfaceCapabilities->maxImageCount = g_ahb_buffer_count;
    pSurfaceCapabilities->currentExtent.width  = desc.width;
    pSurfaceCapabilities->currentExtent.height = desc.height;
    pSurfaceCapabilities->minImageExtent.width  = desc.width;
    pSurfaceCapabilities->minImageExtent.height = desc.height;
    pSurfaceCapabilities->maxImageExtent.width  = desc.width;
    pSurfaceCapabilities->maxImageExtent.height = desc.height;
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform    = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;

    LOGI("layer_GetPhysicalDeviceSurfaceCapabilitiesKHR: returning %ux%u, count=%u",
         desc.width, desc.height, g_ahb_buffer_count);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
    if (!g_ahb_active) {
        return g_inst_dispatch.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    }

    /* Report both RGBA8 and BGRA8 to satisfy DXVK which prefers BGRA8.
     * AHB format AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM maps to VK_FORMAT_R8G8B8A8_UNORM
     * but the Vulkan driver also accepts B8G8R8A8 for AHB import on most devices. */
    static const VkSurfaceFormatKHR formats[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t count = sizeof(formats) / sizeof(formats[0]);

    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = count;
        return VK_SUCCESS;
    }

    uint32_t to_copy = *pSurfaceFormatCount < count ? *pSurfaceFormatCount : count;
    memcpy(pSurfaceFormats, formats, to_copy * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = to_copy;

    LOGI("layer_GetPhysicalDeviceSurfaceFormatsKHR: returning %u formats", to_copy);
    return (to_copy < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
    if (!g_ahb_active) {
        return g_inst_dispatch.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }

    /* Support FIFO (always available) and MAILBOX (for low-latency) */
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t count = sizeof(modes) / sizeof(modes[0]);

    if (!pPresentModes) {
        *pPresentModeCount = count;
        return VK_SUCCESS;
    }

    uint32_t to_copy = *pPresentModeCount < count ? *pPresentModeCount : count;
    memcpy(pPresentModes, modes, to_copy * sizeof(VkPresentModeKHR));
    *pPresentModeCount = to_copy;

    LOGI("layer_GetPhysicalDeviceSurfacePresentModesKHR: returning %u modes", to_copy);
    return (to_copy < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ========================================================================
 * Intercepted device-level functions
 * ======================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    if (!g_ahb_active || g_ahb_buffer_count == 0) {
        return g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    LOGI("layer_CreateSwapchainKHR: request %dx%d fmt=%d mode=%d minCount=%u oldSwapchain=%p",
         pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
         pCreateInfo->imageFormat, pCreateInfo->presentMode,
         pCreateInfo->minImageCount, (void*)(uintptr_t)pCreateInfo->oldSwapchain);

    /* Handle oldSwapchain: if it's our AHB swapchain, destroy it first */
    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE &&
        g_ahb_swapchain &&
        g_real_swapchain_handle == pCreateInfo->oldSwapchain) {
        LOGI("layer_CreateSwapchainKHR: retiring old AHB swapchain");
        wine_ahb_destroy_swapchain(g_ahb_swapchain);
        g_ahb_swapchain = NULL;
        /* Don't destroy the real swapchain here; pass it as oldSwapchain below */
    }

    /* === THE SNIPER HOOK ===
     * Only intercept swapchains that EXACTLY match the AHB pool dimensions.
     * All other swapchains (DXVK probing, UI overlays, different resolutions)
     * pass through to the real Turnip driver. This lets DXVK's format/mode
     * probing phase succeed normally against the real driver, and we only
     * hijack the final gameplay swapchain. */
    AHardwareBuffer_Desc ahb_desc;
    AHardwareBuffer_describe(g_ahb_buffers[0], &ahb_desc);
    if (pCreateInfo->imageExtent.width != ahb_desc.width ||
        pCreateInfo->imageExtent.height != ahb_desc.height) {
        LOGI("layer_CreateSwapchainKHR: PASSTHROUGH (%dx%d != AHB %dx%d)",
             pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
             ahb_desc.width, ahb_desc.height);
        return g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    LOGI("layer_CreateSwapchainKHR: SNIPER HOOK! (%dx%d matches AHB pool) — intercepting",
         pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);

    struct wine_vk_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

    static struct wine_vk_surface fake_surface;
    fake_surface.socket_fd    = g_ahb_socket_fd;
    fake_surface.width        = (int)pCreateInfo->imageExtent.width;
    fake_surface.height       = (int)pCreateInfo->imageExtent.height;
    fake_surface.ahb_supported = true;

    sc->surface      = &fake_surface;
    sc->device       = device;
    sc->image_count  = (g_ahb_buffer_count < AHB_MAX_IMAGES) ? g_ahb_buffer_count : AHB_MAX_IMAGES;
    sc->current_image = 0;

    /* Resolve device function pointers from the next layer/driver */
    PFN_vkGetDeviceProcAddr gdpa = g_dev_dispatch.GetDeviceProcAddr;
    sc->pfn_vkGetFenceFdKHR    = (PFN_vkGetFenceFdKHR)   gdpa(device, "vkGetFenceFdKHR");
    sc->pfn_vkImportFenceFdKHR = (PFN_vkImportFenceFdKHR)gdpa(device, "vkImportFenceFdKHR");
    sc->pfn_vkCreateFence      = (PFN_vkCreateFence)      gdpa(device, "vkCreateFence");
    sc->pfn_vkDestroyFence     = (PFN_vkDestroyFence)     gdpa(device, "vkDestroyFence");
    sc->pfn_vkWaitForFences    = (PFN_vkWaitForFences)    gdpa(device, "vkWaitForFences");
    sc->pfn_vkResetFences      = (PFN_vkResetFences)      gdpa(device, "vkResetFences");
    sc->pfn_vkAllocateMemory   = (PFN_vkAllocateMemory)   gdpa(device, "vkAllocateMemory");
    sc->pfn_vkFreeMemory       = (PFN_vkFreeMemory)       gdpa(device, "vkFreeMemory");
    sc->pfn_vkCreateImage      = (PFN_vkCreateImage)      gdpa(device, "vkCreateImage");
    sc->pfn_vkDestroyImage     = (PFN_vkDestroyImage)     gdpa(device, "vkDestroyImage");
    sc->pfn_vkBindImageMemory2 = (PFN_vkBindImageMemory2) gdpa(device, "vkBindImageMemory2");
    sc->pfn_vkGetDeviceProcAddr = gdpa;

    /* Create reuse fences */
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                             .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->pfn_vkCreateFence(device, &fi, NULL, &sc->images[i].reuse_fence) != VK_SUCCESS) {
            LOGE("layer_CreateSwapchainKHR: vkCreateFence failed slot %u", i);
            free(sc);
            return g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        sc->images[i].release_fence = -1;
        sc->images[i].in_use = false;
    }

    /* Import AHBs */
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps = g_inst_dispatch.GetPhysicalDeviceMemoryProperties;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult ires = import_ahb_to_vk_image(sc, i, g_ahb_buffers[i], g_phys_device, getMemProps);
        if (ires != VK_SUCCESS) {
            LOGE("layer_CreateSwapchainKHR: import failed slot %u: %d", i, ires);
            for (uint32_t j = 0; j < i; j++) {
                sc->pfn_vkDestroyImage(device, sc->images[j].vk_image, NULL);
                sc->pfn_vkFreeMemory(device, sc->images[j].vk_memory, NULL);
            }
            for (uint32_t j = 0; j < sc->image_count; j++)
                sc->pfn_vkDestroyFence(device, sc->images[j].reuse_fence, NULL);
            free(sc);
            LOGW("layer_CreateSwapchainKHR: AHB import failed, falling back");
            return g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        LOGI("layer_CreateSwapchainKHR: slot %u imported (image=%p)", i, (void*)sc->images[i].vk_image);
    }

    g_ahb_swapchain = sc;

    /* === TROJAN HORSE: create a REAL swapchain for handle validity === */
    /* Override present mode to MAILBOX/IMMEDIATE so the trojan never blocks
     * on acquire (FIFO would throttle to vsync, stalling our pipeline). */
    VkSwapchainCreateInfoKHR trojan_info = *pCreateInfo;
    trojan_info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    VkResult real_res = g_dev_dispatch.CreateSwapchainKHR(device, &trojan_info, pAllocator, pSwapchain);
    if (real_res != VK_SUCCESS) {
        /* MAILBOX not supported, try IMMEDIATE */
        trojan_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        real_res = g_dev_dispatch.CreateSwapchainKHR(device, &trojan_info, pAllocator, pSwapchain);
    }
    if (real_res != VK_SUCCESS) {
        /* Fall back to original present mode */
        real_res = g_dev_dispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (real_res != VK_SUCCESS) {
        LOGE("layer_CreateSwapchainKHR: real swapchain creation failed (%d), using AHB-only", real_res);
        /* Fallback: return fake handle (may still fail with Wine thunks) */
        *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    } else {
        g_real_swapchain_handle = *pSwapchain;
        LOGI("layer_CreateSwapchainKHR: real swapchain handle=%p stored as trojan",
             (void*)(uintptr_t)*pSwapchain);

        /* Get trojan swapchain images — these are regular device-local images
         * (no AHB backing) that DXVK will render into safely. */
        g_trojan_image_count = AHB_MAX_IMAGES;
        VkResult img_res = g_dev_dispatch.GetSwapchainImagesKHR(
            device, g_real_swapchain_handle, &g_trojan_image_count, g_trojan_images);
        if (img_res != VK_SUCCESS && img_res != VK_INCOMPLETE) {
            LOGE("layer_CreateSwapchainKHR: failed to get trojan images (%d)", img_res);
            g_trojan_image_count = 0;
        } else {
            LOGI("layer_CreateSwapchainKHR: got %u trojan images for blit indirection",
                 g_trojan_image_count);
        }

        /* Create command pool + buffer + fence for the blit copy */
        if (g_trojan_image_count > 0 && !g_copy_cmd_pool) {
            VkCommandPoolCreateInfo pool_ci = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = 0, /* graphics queue family */
            };
            g_dev_dispatch.CreateCommandPool(device, &pool_ci, NULL, &g_copy_cmd_pool);

            VkCommandBufferAllocateInfo alloc_ci = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = g_copy_cmd_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            g_dev_dispatch.AllocateCommandBuffers(device, &alloc_ci, &g_copy_cmd_buf);

            VkFenceCreateInfo fence_ci = {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            g_dev_dispatch.CreateFence(device, &fence_ci, NULL, &g_copy_fence);
            LOGI("layer_CreateSwapchainKHR: blit resources created (pool=%p, cmd=%p, fence=%p)",
                 (void*)g_copy_cmd_pool, (void*)g_copy_cmd_buf, (void*)g_copy_fence);
        }
    }

    LOGI("layer_CreateSwapchainKHR: AHB SWAPCHAIN CREATED (%dx%d, %u images) — BLIT INDIRECTION %s",
         pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, sc->image_count,
         g_trojan_image_count > 0 ? "ACTIVE" : "DISABLED");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    if (g_ahb_swapchain && g_real_swapchain_handle == swapchain) {
        LOGI("layer_DestroySwapchainKHR: destroying AHB swapchain + real handle");
        wine_ahb_destroy_swapchain(g_ahb_swapchain);
        g_ahb_swapchain = NULL;
        /* Also destroy the real swapchain handle */
        g_dev_dispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
        g_real_swapchain_handle = VK_NULL_HANDLE;
        return;
    }
    g_dev_dispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pCount, VkImage *pImages)
{
    if (g_ahb_swapchain && g_real_swapchain_handle == swapchain) {
        /* BLIT INDIRECTION: return trojan images (device-local, no AHB) so DXVK
         * renders into them without gralloc lock conflicts. We copy to AHB in
         * QueuePresentKHR. */
        if (g_trojan_image_count > 0) {
            if (!pImages) {
                *pCount = g_trojan_image_count;
                return VK_SUCCESS;
            }
            uint32_t to_copy = *pCount < g_trojan_image_count ? *pCount : g_trojan_image_count;
            for (uint32_t i = 0; i < to_copy; i++)
                pImages[i] = g_trojan_images[i];
            *pCount = to_copy;
            LOGI("layer_GetSwapchainImagesKHR: returning %u TROJAN images (blit indirection)", to_copy);
            return (to_copy < g_trojan_image_count) ? VK_INCOMPLETE : VK_SUCCESS;
        }
        /* Fallback: return AHB images directly (no blit indirection) */
        VkResult r = wine_ahb_get_swapchain_images(g_ahb_swapchain, pCount, pImages);
        LOGI("layer_GetSwapchainImagesKHR: count=%u images=%p result=%d",
             pCount ? *pCount : 0, (void*)pImages, r);
        return r;
    }
    return g_dev_dispatch.GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
}

/* Track the last real_idx from AcquireNextImageKHR for presenting back to trojan */
static uint32_t g_last_real_idx = 0;
static VkQueue g_saved_queue = VK_NULL_HANDLE;

/* Track last 2 presented slots to avoid returning buffers still held by SurfaceFlinger */
static int g_presented_ring[2] = {-1, -1};
static int g_presented_idx = 0;

static VKAPI_ATTR VkResult VKAPI_CALL layer_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    (void)timeout;
    if (g_ahb_swapchain && g_real_swapchain_handle == swapchain) {
        static int _frames_in_flight = 0;
        uint32_t ahb_idx;

        if (_frames_in_flight >= 2) {
            /* Block until Android releases a buffer (vsync back-pressure) */
            struct release_msg rel;
            ssize_t n = recv(g_ahb_socket_fd, &rel, sizeof(rel), 0);
            if (n == (ssize_t)sizeof(rel) && rel.type == MSG_RELEASE) {
                ahb_idx = rel.slot_index;
                LOGI("layer_AcquireNextImageKHR: recv release slot=%u (frame %d)",
                     ahb_idx, _frames_in_flight + 1);
            } else {
                /* Fallback: recv failed or unexpected message — use ring logic */
                if (n <= 0) {
                    LOGW("layer_AcquireNextImageKHR: recv failed (%zd): %s",
                         n, strerror(errno));
                } else {
                    LOGW("layer_AcquireNextImageKHR: unexpected msg type=%d size=%zd",
                         (int)((uint8_t*)&rel)[0], n);
                }
                ahb_idx = g_ahb_swapchain->current_image;
                for (int attempts = 0; attempts < (int)g_ahb_swapchain->image_count; attempts++) {
                    if ((int)ahb_idx != g_presented_ring[0] && (int)ahb_idx != g_presented_ring[1])
                        break;
                    ahb_idx = (ahb_idx + 1) % g_ahb_swapchain->image_count;
                }
                g_ahb_swapchain->current_image = (ahb_idx + 1) % g_ahb_swapchain->image_count;
            }
        } else {
            /* Bootstrap: first 2 frames don't block (fill the pipeline) */
            ahb_idx = g_ahb_swapchain->current_image;
            for (int attempts = 0; attempts < (int)g_ahb_swapchain->image_count; attempts++) {
                if ((int)ahb_idx != g_presented_ring[0] && (int)ahb_idx != g_presented_ring[1])
                    break;
                ahb_idx = (ahb_idx + 1) % g_ahb_swapchain->image_count;
            }
            g_ahb_swapchain->current_image = (ahb_idx + 1) % g_ahb_swapchain->image_count;
        }
        _frames_in_flight++;

        *pImageIndex = ahb_idx;

        /* Signal the semaphore/fence directly via an empty queue submit */
        if (g_saved_queue && (semaphore || fence)) {
            VkSubmitInfo submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = NULL,
                .waitSemaphoreCount = 0,
                .pWaitSemaphores = NULL,
                .pWaitDstStageMask = NULL,
                .commandBufferCount = 0,
                .pCommandBuffers = NULL,
                .signalSemaphoreCount = semaphore ? 1u : 0u,
                .pSignalSemaphores = semaphore ? &semaphore : NULL,
            };
            g_dev_dispatch.QueueSubmit(g_saved_queue, 1, &submit, fence);
        }

        static int _cnt = 0;
        if (++_cnt <= 5 || (_cnt % 120 == 0))
            LOGI("layer_AcquireNextImageKHR: frame %d ahb_idx=%u",
                 _cnt, ahb_idx);

        return VK_SUCCESS;
    }
    return g_dev_dispatch.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    if (g_ahb_swapchain) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            if (g_real_swapchain_handle == pPresentInfo->pSwapchains[i]) {
                /* Save queue for semaphore signaling in AcquireNextImageKHR */
                if (!g_saved_queue) g_saved_queue = queue;

                uint32_t ahb_slot = pPresentInfo->pImageIndices[i];

                /* Wait for rendering to complete before sending present_msg.
                 * Submit the wait semaphores with no work to drain them. */
                if (pPresentInfo->waitSemaphoreCount > 0) {
                    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
                    VkPipelineStageFlags wait_stages[8];
                    for (uint32_t w = 0; w < pPresentInfo->waitSemaphoreCount && w < 8; w++)
                        wait_stages[w] = wait_stage;
                    VkSubmitInfo drain = {
                        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = pPresentInfo->waitSemaphoreCount,
                        .pWaitSemaphores = pPresentInfo->pWaitSemaphores,
                        .pWaitDstStageMask = wait_stages,
                        .commandBufferCount = 0,
                        .signalSemaphoreCount = 0,
                    };
                    g_dev_dispatch.QueueSubmit(queue, 1, &drain, VK_NULL_HANDLE);
                }

                /* Track this slot as recently presented */
                g_presented_ring[g_presented_idx] = (int)ahb_slot;
                g_presented_idx = (g_presented_idx + 1) % 2;

                /* Send present_msg to Android receiver for display */
                wine_ahb_queue_present(g_ahb_swapchain, VK_NULL_HANDLE, ahb_slot, VK_NULL_HANDLE);

                static int _cnt = 0;
                if (++_cnt <= 5 || (_cnt % 60 == 0))
                    LOGI("layer_QueuePresentKHR: frame %d slot=%u",
                         _cnt, ahb_slot);
                if (pPresentInfo->pResults) pPresentInfo->pResults[i] = VK_SUCCESS;
                return VK_SUCCESS;
            }
        }
    }
    return g_dev_dispatch.QueuePresentKHR(queue, pPresentInfo);
}

/* ========================================================================
 * Layer core: vkCreateInstance
 * ======================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *layerInfo = get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!layerInfo) {
        LOGE("layer_CreateInstance: no layer link info");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    /* Advance the chain for the next layer */
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)
        fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!fpCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    g_instance = *pInstance;

    /* Populate instance dispatch */
    g_inst_dispatch.GetInstanceProcAddr = fpGetInstanceProcAddr;
    g_inst_dispatch.DestroyInstance = (PFN_vkDestroyInstance)
        fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    g_inst_dispatch.EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)
        fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices");
    g_inst_dispatch.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)
        fpGetInstanceProcAddr(*pInstance, "vkEnumerateDeviceExtensionProperties");
    g_inst_dispatch.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
    g_inst_dispatch.CreateDevice = (PFN_vkCreateDevice)
        fpGetInstanceProcAddr(*pInstance, "vkCreateDevice");
    g_inst_dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    g_inst_dispatch.GetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    g_inst_dispatch.GetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    LOGI("layer_CreateInstance: instance=%p chain OK", (void*)*pInstance);
    return VK_SUCCESS;
}

/* ========================================================================
 * Layer core: vkCreateDevice
 * ======================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *layerInfo = get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!layerInfo) {
        LOGE("layer_CreateDevice: no layer link info");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    /* Advance the chain */
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;

    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)
        fpGetInstanceProcAddr(g_instance, "vkCreateDevice");
    if (!fpCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    g_device = *pDevice;
    g_phys_device = physicalDevice;

    /* Populate device dispatch */
    g_dev_dispatch.GetDeviceProcAddr = fpGetDeviceProcAddr;
    g_dev_dispatch.DestroyDevice = (PFN_vkDestroyDevice)
        fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    g_dev_dispatch.CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)
        fpGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
    g_dev_dispatch.DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)
        fpGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
    g_dev_dispatch.GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)
        fpGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
    g_dev_dispatch.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)
        fpGetDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR");
    g_dev_dispatch.QueuePresentKHR = (PFN_vkQueuePresentKHR)
        fpGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
    g_dev_dispatch.QueueSubmit = (PFN_vkQueueSubmit)
        fpGetDeviceProcAddr(*pDevice, "vkQueueSubmit");
    g_dev_dispatch.CreateCommandPool = (PFN_vkCreateCommandPool)
        fpGetDeviceProcAddr(*pDevice, "vkCreateCommandPool");
    g_dev_dispatch.AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)
        fpGetDeviceProcAddr(*pDevice, "vkAllocateCommandBuffers");
    g_dev_dispatch.BeginCommandBuffer = (PFN_vkBeginCommandBuffer)
        fpGetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer");
    g_dev_dispatch.EndCommandBuffer = (PFN_vkEndCommandBuffer)
        fpGetDeviceProcAddr(*pDevice, "vkEndCommandBuffer");
    g_dev_dispatch.CmdCopyImage = (PFN_vkCmdCopyImage)
        fpGetDeviceProcAddr(*pDevice, "vkCmdCopyImage");
    g_dev_dispatch.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)
        fpGetDeviceProcAddr(*pDevice, "vkCmdPipelineBarrier");
    g_dev_dispatch.CreateFence = (PFN_vkCreateFence)
        fpGetDeviceProcAddr(*pDevice, "vkCreateFence");
    g_dev_dispatch.DestroyFence = (PFN_vkDestroyFence)
        fpGetDeviceProcAddr(*pDevice, "vkDestroyFence");
    g_dev_dispatch.WaitForFences = (PFN_vkWaitForFences)
        fpGetDeviceProcAddr(*pDevice, "vkWaitForFences");
    g_dev_dispatch.ResetFences = (PFN_vkResetFences)
        fpGetDeviceProcAddr(*pDevice, "vkResetFences");
    g_dev_dispatch.ResetCommandBuffer = (PFN_vkResetCommandBuffer)
        fpGetDeviceProcAddr(*pDevice, "vkResetCommandBuffer");
    g_dev_dispatch.DestroyCommandPool = (PFN_vkDestroyCommandPool)
        fpGetDeviceProcAddr(*pDevice, "vkDestroyCommandPool");
    g_dev_dispatch.QueueWaitIdle = (PFN_vkQueueWaitIdle)
        fpGetDeviceProcAddr(*pDevice, "vkQueueWaitIdle");

    /* Get queue 0 for semaphore signaling in AcquireNextImageKHR */
    {
        PFN_vkGetDeviceQueue getQueue = (PFN_vkGetDeviceQueue)
            fpGetDeviceProcAddr(*pDevice, "vkGetDeviceQueue");
        if (getQueue) {
            getQueue(*pDevice, 0, 0, &g_saved_queue);
        }
    }

    /* Connect to AHB server */
    connect_ahb();

    /* Patch the device dispatch table to ensure vkQueuePresentKHR routes through
     * our layer even when Wine/winevulkan caches function pointers from the
     * device's internal dispatch table (bypassing GetDeviceProcAddr). */
    {
        void **dispatch = *(void***)(*pDevice);
        if (dispatch) {
            void *real_present = (void*)g_dev_dispatch.QueuePresentKHR;
            int patched = 0;
            for (int i = 0; i < 512 && !patched; i++) {
                if (dispatch[i] == real_present) {
                    dispatch[i] = (void*)layer_QueuePresentKHR;
                    patched = 1;
                    LOGI("layer_CreateDevice: patched dispatch[%d] vkQueuePresentKHR → layer", i);
                }
            }
            if (!patched)
                LOGW("layer_CreateDevice: could not find vkQueuePresentKHR in device dispatch");
        }
    }

    LOGI("layer_CreateDevice: device=%p AHB active=%d", (void*)*pDevice, g_ahb_active);
    return VK_SUCCESS;
}

/* ========================================================================
 * GetInstanceProcAddr / GetDeviceProcAddr
 * ======================================================================== */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetInstanceProcAddr(VkInstance instance, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    if (!pName) return NULL;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)      return (PFN_vkVoidFunction)AHBLayer_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)     return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)    return (PFN_vkVoidFunction)layer_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)  return (PFN_vkVoidFunction)layer_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)    return (PFN_vkVoidFunction)layer_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)        return (PFN_vkVoidFunction)layer_QueuePresentKHR;

    if (g_dev_dispatch.GetDeviceProcAddr)
        return g_dev_dispatch.GetDeviceProcAddr(device, pName);
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (!pName) return NULL;

    if (strcmp(pName, "vkCreateInstance") == 0)          return (PFN_vkVoidFunction)layer_CreateInstance;
    if (strcmp(pName, "vkCreateDevice") == 0)            return (PFN_vkVoidFunction)layer_CreateDevice;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)     return (PFN_vkVoidFunction)AHBLayer_GetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)       return (PFN_vkVoidFunction)AHBLayer_GetDeviceProcAddr;

    /* Surface query intercepts DISABLED — let Turnip handle all surface queries
     * so DXVK gets real driver capabilities. We only intercept the final gameplay
     * swapchain creation (the "sniper hook" in layer_CreateSwapchainKHR). */
    /* if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)  return (PFN_vkVoidFunction)layer_GetPhysicalDeviceSurfaceCapabilitiesKHR; */
    /* if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)       return (PFN_vkVoidFunction)layer_GetPhysicalDeviceSurfaceFormatsKHR; */
    /* if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)  return (PFN_vkVoidFunction)layer_GetPhysicalDeviceSurfacePresentModesKHR; */

    /* Device-level intercepts also need to be returned from GIPA */
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)     return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)    return (PFN_vkVoidFunction)layer_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)  return (PFN_vkVoidFunction)layer_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)    return (PFN_vkVoidFunction)layer_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)        return (PFN_vkVoidFunction)layer_QueuePresentKHR;

    if (g_inst_dispatch.GetInstanceProcAddr)
        return g_inst_dispatch.GetInstanceProcAddr(instance, pName);
    return NULL;
}

/* ========================================================================
 * Mandatory layer exports
 * ======================================================================== */

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1) {
        *pPropertyCount = 1;
        return VK_INCOMPLETE;
    }
    *pPropertyCount = 1;
    memset(pProperties, 0, sizeof(*pProperties));
    strncpy(pProperties->layerName, "VK_LAYER_WINLATOR_ahb_direct", VK_MAX_EXTENSION_NAME_SIZE);
    strncpy(pProperties->description, "Winlator AHB Direct Compositing layer", VK_MAX_DESCRIPTION_SIZE);
    pProperties->specVersion = VK_MAKE_VERSION(1, 3, 0);
    pProperties->implementationVersion = 1;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    if (pLayerName && strcmp(pLayerName, "VK_LAYER_WINLATOR_ahb_direct") == 0) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    /* Not our layer, return nothing */
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

/* Named entry points referenced in the JSON manifest */
__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetInstanceProcAddr_Export(VkInstance instance, const char *pName)
{
    return AHBLayer_GetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AHBLayer_GetDeviceProcAddr_Export(VkDevice device, const char *pName)
{
    return AHBLayer_GetDeviceProcAddr(device, pName);
}

/* Constructor for diagnostics */
__attribute__((constructor))
static void ahb_layer_ctor(void)
{
    LOGI("[CTOR] libahb_layer.so loaded in PID=%d UID=%d", getpid(), getuid());
}
