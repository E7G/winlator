/*
 * AHB Direct Compositing - Vulkan ICD Wrapper ("Perfect Mirror")
 *
 * Replaces libvulkan_wrapper.so. Forwards all calls to the real driver
 * (libvulkan_wrapper_real.so), intercepting only swapchain operations.
 *
 * Key design:
 *  - Exports vk_icdNegotiateLoaderICDInterfaceVersion (caps at v5)
 *  - Exports vk_icdGetInstanceProcAddr (intercepts 6 functions)
 *  - Exports vk_icdGetPhysicalDeviceProcAddr (required at v5+)
 *  - All other functions forwarded to real driver via g_real_gipa
 *  - connect_ahb() called lazily on first vkCreateDevice
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
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "../wineandroid.drv/vulkan_ahb.h"

#define LOG_TAG "AHB_ICD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ========================================================================
 * Diagnostic helpers
 * ======================================================================== */

static void read_self_cmdline(char *out, size_t out_sz)
{
    out[0] = '\0';
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    size_t n = fread(out, 1, out_sz - 1, f);
    fclose(f);
    /* cmdline is NUL-separated; replace NULs with spaces for logging */
    for (size_t i = 0; i + 1 < n; i++) {
        if (out[i] == '\0') out[i] = ' ';
    }
    out[n] = '\0';
}

__attribute__((constructor))
static void ahb_icd_ctor(void)
{
    char cmd[512] = {0};
    read_self_cmdline(cmd, sizeof(cmd));
    LOGI("[CTOR] libvulkan_wrapper.so (AHB ICD) loaded in PID=%d UID=%d cmdline='%s'",
         getpid(), getuid(), cmd);
}

static const char *self_cmdline_cached(void)
{
    static char cached[512] = {0};
    static int  cached_ok   = 0;
    if (!cached_ok) {
        read_self_cmdline(cached, sizeof(cached));
        cached_ok = 1;
    }
    return cached;
}

/* ========================================================================
 * Global state
 * ======================================================================== */

static void  *g_real_icd  = NULL;
static PFN_vkGetInstanceProcAddr g_real_gipa = NULL;
static PFN_vkGetDeviceProcAddr   g_real_gdpa = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties g_real_GetPhysMemProps = NULL;

static int    g_ahb_socket_fd    = -1;
static int    g_ahb_active       = 0;
static int    g_ahb_connected    = 0;
static struct wine_vk_swapchain *g_ahb_swapchain = NULL;
static VkDevice         g_device      = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys_device = VK_NULL_HANDLE;

static AHardwareBuffer *g_ahb_buffers[3] = {NULL, NULL, NULL};
static int g_ahb_buffer_count = 0;

/* ========================================================================
 * Real ICD loading
 * ======================================================================== */

static int ensure_real_icd(void)
{
    if (g_real_icd) return 1;

    g_real_icd = dlopen("libvulkan_wrapper_real.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_real_icd) {
        LOGE("ensure_real_icd: dlopen failed: %s", dlerror());
        return 0;
    }

    /* Prefer the ICD-specific entry point */
    g_real_gipa = (PFN_vkGetInstanceProcAddr)
        dlsym(g_real_icd, "vk_icdGetInstanceProcAddr");
    if (!g_real_gipa)
        g_real_gipa = (PFN_vkGetInstanceProcAddr)
            dlsym(g_real_icd, "vkGetInstanceProcAddr");

    if (!g_real_gipa) {
        LOGE("ensure_real_icd: no entry point found");
        dlclose(g_real_icd);
        g_real_icd = NULL;
        return 0;
    }

    LOGI("ensure_real_icd: loaded libvulkan_wrapper_real.so (caller PID=%d cmdline='%s')",
         getpid(), self_cmdline_cached());
    return 1;
}

/* ========================================================================
 * AHB socket connection (deferred to first vkCreateDevice)
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

    for (int i = 0; i < 3; i++) {
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
 * Forward declarations
 * ======================================================================== */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrap_GetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice);

/* ========================================================================
 * Intercepted Vulkan functions
 * ======================================================================== */

/*
 * wrap_CreateInstance — intercepts vkCreateInstance so we can patch the
 * returned VkInstance's dispatch table to redirect vkCreateDevice and
 * vkGetDeviceProcAddr through our wrappers.
 *
 * Wine's Vulkan bridge calls vk_icdGetInstanceProcAddr exactly twice
 * (for vkCreateInstance + vkEnumerateInstanceExtensionProperties) and
 * then resolves ALL subsequent functions via the created VkInstance's own
 * dispatch table — bypassing our ICD GIPA entirely.  By replacing the
 * dispatch table entries for vkCreateDevice and vkGetDeviceProcAddr we
 * ensure our wrappers are called even under Wine's non-standard dispatch.
 */
static VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    /* DIAGNOSTIC: log the very first thing — if this never appears, the function
     * is returned from GIPA but never actually called (ARM64 loader or Box64 is
     * using a different code path to get to vkCreateInstance). */
    LOGI("wrap_CreateInstance: ENTER (pCreateInfo=%p pInstance=%p)",
         (void*)pCreateInfo, (void*)pInstance);

    PFN_vkCreateInstance real_fn = (PFN_vkCreateInstance)
        g_real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!real_fn) {
        LOGE("wrap_CreateInstance: real vkCreateInstance not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = real_fn(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS || !pInstance || !*pInstance) {
        LOGE("wrap_CreateInstance: real vkCreateInstance returned %d", res);
        return res;
    }

    /* Patch the instance dispatch table so Wine's bridge calls our
     * wrap_CreateDevice and wrap_GetDeviceProcAddr instead of the real
     * driver's versions.
     *
     * VkInstance_T layout (ARM64 Turnip / any Khronos-spec ICD):
     *   offset 0: pointer to the dispatch table  <-- void **dispatch
     * dispatch[0..N]: one function pointer per Vulkan function
     */
    void **dispatch = *(void***)(*pInstance);
    if (dispatch) {
        /* Obtain real driver function pointers to scan for in the table */
        void *real_CreateDevice = (void *)g_real_gipa(VK_NULL_HANDLE, "vkCreateDevice");
        void *real_GDPA         = (void *)g_real_gipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr");

        int patched_cd = 0, patched_gdpa = 0;
        for (int i = 0; i < 512; i++) {
            if (!dispatch[i]) continue;
            if (!patched_cd && real_CreateDevice && dispatch[i] == real_CreateDevice) {
                LOGI("wrap_CreateInstance: patched dispatch[%d] vkCreateDevice → wrap", i);
                dispatch[i] = (void*)wrap_CreateDevice;
                patched_cd  = 1;
            }
            if (!patched_gdpa && real_GDPA && dispatch[i] == real_GDPA) {
                LOGI("wrap_CreateInstance: patched dispatch[%d] vkGetDeviceProcAddr → wrap", i);
                dispatch[i]  = (void*)wrap_GetDeviceProcAddr;
                patched_gdpa = 1;
            }
            if (patched_cd && patched_gdpa) break;
        }
        if (!patched_cd)
            LOGW("wrap_CreateInstance: could not find vkCreateDevice in dispatch (real=%p)",
                 real_CreateDevice);
        if (!patched_gdpa)
            LOGW("wrap_CreateInstance: could not find vkGetDeviceProcAddr in dispatch");
    } else {
        LOGW("wrap_CreateInstance: dispatch table NULL — cannot patch");
    }

    LOGI("wrap_CreateInstance: instance=%p dispatch patched", (void*)*pInstance);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    /* Resolve vkCreateDevice from the real driver via the instance */
    PFN_vkCreateDevice real_fn = (PFN_vkCreateDevice)
        g_real_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!real_fn) {
        LOGE("wrap_CreateDevice: real vkCreateDevice not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = real_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    g_device      = *pDevice;
    g_phys_device = physicalDevice;

    /* Get real device-level proc addr */
    g_real_gdpa = (PFN_vkGetDeviceProcAddr)
        g_real_gipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    g_real_GetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        g_real_gipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");

    /* Connect to AHB server — only fires once, only for real Vulkan processes */
    connect_ahb();

    LOGI("wrap_CreateDevice: device=%p AHB active=%d", (void*)*pDevice, g_ahb_active);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL wrap_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    if (g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        LOGI("wrap_DestroySwapchainKHR: destroying AHB swapchain");
        wine_ahb_destroy_swapchain(g_ahb_swapchain);
        g_ahb_swapchain = NULL;
        return;
    }
    if (g_real_gdpa) {
        PFN_vkDestroySwapchainKHR fn = (PFN_vkDestroySwapchainKHR)
            g_real_gdpa(device, "vkDestroySwapchainKHR");
        if (fn) fn(device, swapchain, pAllocator);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    PFN_vkCreateSwapchainKHR real_fn = g_real_gdpa
        ? (PFN_vkCreateSwapchainKHR)g_real_gdpa(device, "vkCreateSwapchainKHR")
        : NULL;

    /* Passthrough if AHB not active or no buffers */
    if (!g_ahb_active || g_ahb_buffer_count == 0 || !real_fn) {
        if (real_fn) return real_fn(device, pCreateInfo, pAllocator, pSwapchain);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Skip tiny swapchains (UI overlays etc.) */
    if (pCreateInfo->imageExtent.width < 64 || pCreateInfo->imageExtent.height < 64) {
        LOGI("wrap_CreateSwapchainKHR: small (%dx%d), passthrough",
             pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
        return real_fn(device, pCreateInfo, pAllocator, pSwapchain);
    }

    struct wine_vk_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return real_fn(device, pCreateInfo, pAllocator, pSwapchain);

    static struct wine_vk_surface fake_surface;
    fake_surface.socket_fd    = g_ahb_socket_fd;
    fake_surface.width        = (int)pCreateInfo->imageExtent.width;
    fake_surface.height       = (int)pCreateInfo->imageExtent.height;
    fake_surface.ahb_supported = true;

    sc->surface      = &fake_surface;
    sc->device       = device;
    sc->image_count  = (g_ahb_buffer_count < 3) ? g_ahb_buffer_count : 3;
    sc->current_image = 0;

    /* Load device function pointers from real driver */
    sc->pfn_vkGetFenceFdKHR    = (PFN_vkGetFenceFdKHR)   g_real_gdpa(device, "vkGetFenceFdKHR");
    sc->pfn_vkImportFenceFdKHR = (PFN_vkImportFenceFdKHR) g_real_gdpa(device, "vkImportFenceFdKHR");
    sc->pfn_vkCreateFence      = (PFN_vkCreateFence)      g_real_gdpa(device, "vkCreateFence");
    sc->pfn_vkDestroyFence     = (PFN_vkDestroyFence)     g_real_gdpa(device, "vkDestroyFence");
    sc->pfn_vkWaitForFences    = (PFN_vkWaitForFences)    g_real_gdpa(device, "vkWaitForFences");
    sc->pfn_vkResetFences      = (PFN_vkResetFences)      g_real_gdpa(device, "vkResetFences");
    sc->pfn_vkAllocateMemory   = (PFN_vkAllocateMemory)   g_real_gdpa(device, "vkAllocateMemory");
    sc->pfn_vkFreeMemory       = (PFN_vkFreeMemory)       g_real_gdpa(device, "vkFreeMemory");
    sc->pfn_vkCreateImage      = (PFN_vkCreateImage)      g_real_gdpa(device, "vkCreateImage");
    sc->pfn_vkDestroyImage     = (PFN_vkDestroyImage)     g_real_gdpa(device, "vkDestroyImage");
    sc->pfn_vkBindImageMemory2 = (PFN_vkBindImageMemory2) g_real_gdpa(device, "vkBindImageMemory2");
    sc->pfn_vkGetDeviceProcAddr = g_real_gdpa;

    /* Create reuse fences */
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                             .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->pfn_vkCreateFence(device, &fi, NULL, &sc->images[i].reuse_fence) != VK_SUCCESS) {
            LOGE("wrap_CreateSwapchainKHR: vkCreateFence failed slot %u", i);
            free(sc);
            return real_fn(device, pCreateInfo, pAllocator, pSwapchain);
        }
        sc->images[i].release_fence = -1;
        sc->images[i].in_use = false;
    }

    /* Import each pre-received AHB into Vulkan */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult ires = import_ahb_to_vk_image(sc, i, g_ahb_buffers[i],
                                                g_phys_device, g_real_GetPhysMemProps);
        if (ires != VK_SUCCESS) {
            LOGE("wrap_CreateSwapchainKHR: import failed slot %u: %d", i, ires);
            for (uint32_t j = 0; j < i; j++) {
                sc->pfn_vkDestroyImage(device, sc->images[j].vk_image, NULL);
                sc->pfn_vkFreeMemory(device, sc->images[j].vk_memory, NULL);
            }
            for (uint32_t j = 0; j < sc->image_count; j++)
                sc->pfn_vkDestroyFence(device, sc->images[j].reuse_fence, NULL);
            free(sc);
            LOGW("wrap_CreateSwapchainKHR: AHB import failed, falling back");
            return real_fn(device, pCreateInfo, pAllocator, pSwapchain);
        }
        LOGI("wrap_CreateSwapchainKHR: slot %u imported (image=%p)",
             i, (void*)sc->images[i].vk_image);
    }

    g_ahb_swapchain = sc;
    *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    LOGI("wrap_CreateSwapchainKHR: AHB SWAPCHAIN CREATED! (%dx%d, %u images)",
         pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, sc->image_count);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pCount, VkImage *pImages)
{
    if (g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        VkResult r = wine_ahb_get_swapchain_images(g_ahb_swapchain, pCount, pImages);
        LOGI("wrap_GetSwapchainImagesKHR: count=%u images=%p result=%d",
             pCount ? *pCount : 0, (void*)pImages, r);
        return r;
    }
    PFN_vkGetSwapchainImagesKHR fn = (PFN_vkGetSwapchainImagesKHR)
        g_real_gdpa(device, "vkGetSwapchainImagesKHR");
    return fn(device, swapchain, pCount, pImages);
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    if (g_ahb_swapchain &&
        (VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == swapchain) {
        VkResult r = wine_ahb_acquire_next_image(g_ahb_swapchain, timeout,
                                                  semaphore, fence, pImageIndex);
        static int _cnt = 0;
        if (++_cnt <= 5 || (_cnt % 120 == 0))
            LOGI("wrap_AcquireNextImageKHR: frame %d idx=%u result=%d",
                 _cnt, pImageIndex ? *pImageIndex : 999, r);
        return r;
    }
    PFN_vkAcquireNextImageKHR fn = (PFN_vkAcquireNextImageKHR)
        g_real_gdpa(device, "vkAcquireNextImageKHR");
    return fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    if (g_ahb_swapchain) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            if ((VkSwapchainKHR)(uintptr_t)g_ahb_swapchain == pPresentInfo->pSwapchains[i]) {
                VkResult r = wine_ahb_queue_present(
                    g_ahb_swapchain, queue, pPresentInfo->pImageIndices[i], VK_NULL_HANDLE);
                static int _cnt = 0;
                if (++_cnt <= 5 || (_cnt % 60 == 0))
                    LOGI("wrap_QueuePresentKHR: frame %d slot=%u result=%d",
                         _cnt, pPresentInfo->pImageIndices[i], r);
                if (pPresentInfo->pResults) pPresentInfo->pResults[i] = r;
                return r;
            }
        }
    }
    PFN_vkQueuePresentKHR fn = (PFN_vkQueuePresentKHR)
        g_real_gdpa(g_device, "vkQueuePresentKHR");
    return fn(queue, pPresentInfo);
}

/* ========================================================================
 * Device-level proc addr
 * ======================================================================== */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrap_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)  return (PFN_vkVoidFunction)wrap_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)wrap_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)wrap_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)wrap_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)     return (PFN_vkVoidFunction)wrap_QueuePresentKHR;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)   return (PFN_vkVoidFunction)wrap_GetDeviceProcAddr;
    if (g_real_gdpa) return g_real_gdpa(device, pName);
    return NULL;
}

/* ========================================================================
 * Mandatory ICD exports
 * ======================================================================== */

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (!ensure_real_icd()) {
        LOGE("vk_icdGetInstanceProcAddr(%s): ensure_real_icd FAILED", pName ? pName : "NULL");
        return NULL;
    }

    /* Log every entry for diagnostics — first 100 calls total, keep tight */
    static int _gipa_cnt = 0;
    if (_gipa_cnt < 100) {
        _gipa_cnt++;
        LOGI("vk_icdGetInstanceProcAddr #%d: instance=%p pName='%s'",
             _gipa_cnt, (void*)instance, pName ? pName : "(null)");
    }

    if (!pName) return NULL;

    /* Return OUR vk_icdGetInstanceProcAddr when asked for vkGetInstanceProcAddr
     * so Wine keeps routing through our wrapper for subsequent function lookups. */
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;

    /* Intercept vkCreateInstance so we can patch the returned VkInstance's
     * dispatch table and redirect device creation through our wrappers.
     * This is the critical path: Wine only calls GIPA twice (for vkCreateInstance
     * and vkEnumerateInstanceExtensionProperties) and then uses the VkInstance's
     * own dispatch for everything else — if we don't patch that dispatch, our
     * wrap_CreateDevice and wrap_GetDeviceProcAddr are never called. */
    if (strcmp(pName, "vkCreateInstance") == 0)        return (PFN_vkVoidFunction)wrap_CreateInstance;

    if (strcmp(pName, "vkCreateDevice") == 0)          return (PFN_vkVoidFunction)wrap_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)     return (PFN_vkVoidFunction)wrap_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)    return (PFN_vkVoidFunction)wrap_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)   return (PFN_vkVoidFunction)wrap_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)wrap_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)   return (PFN_vkVoidFunction)wrap_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)       return (PFN_vkVoidFunction)wrap_QueuePresentKHR;

    /* Everything else: real driver */
    PFN_vkVoidFunction fn = g_real_gipa(instance, pName);
    if (_gipa_cnt <= 100 && !fn) {
        LOGW("vk_icdGetInstanceProcAddr: real driver returned NULL for '%s'", pName);
    }
    return fn;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    /* Same as vk_icdGetInstanceProcAddr — some loaders call this directly */
    return vk_icdGetInstanceProcAddr(instance, pName);
}

/* ========================================================================
 * Named exports for global/pre-instance functions
 *
 * At ICD interface v7 the Khronos loader uses dlsym(icd_handle, "vkXxx")
 * for global (pre-instance) functions BEFORE falling back to GIPA.
 * Without these exports the loader silently skips our wrappers and calls
 * the real driver's symbols through a different code path, making our
 * GIPA-returned function pointers dead code.
 * ======================================================================== */

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    LOGI("vkCreateInstance: called via named export (dlsym path)");
    return wrap_CreateInstance(pCreateInfo, pAllocator, pInstance);
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    LOGI("vkCreateDevice: called via named export (dlsym path)");
    return wrap_CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char *pName)
{
    return wrap_GetDeviceProcAddr(device, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    if (!ensure_real_icd()) return NULL;

    static int _pdpa_cnt = 0;
    if (_pdpa_cnt < 40) {
        _pdpa_cnt++;
        LOGI("vk_icdGetPhysicalDeviceProcAddr #%d: instance=%p pName='%s'",
             _pdpa_cnt, (void*)instance, pName ? pName : "(null)");
    }

    typedef PFN_vkVoidFunction (VKAPI_CALL *PFN_GetPhys)(VkInstance, const char*);
    PFN_GetPhys real_fn = (PFN_GetPhys)
        dlsym(g_real_icd, "vk_icdGetPhysicalDeviceProcAddr");
    if (real_fn) return real_fn(instance, pName);
    return g_real_gipa(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    uint32_t loader_max = pSupportedVersion ? *pSupportedVersion : 0;
    LOGI("vk_icdNegotiateLoaderICDInterfaceVersion: ENTER (loader wants up to v%u) PID=%d cmdline='%s'",
         loader_max, getpid(), self_cmdline_cached());

    if (!ensure_real_icd()) {
        LOGE("vk_icdNegotiateLoaderICDInterfaceVersion: ensure_real_icd FAILED");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Let the real driver initialize its internal state and tell us the
     * highest version it supports. We pass that version (real_ver) back to
     * the loader so the loader uses the real driver's dispatch protocol.
     *
     * Previously we hard-capped at v5 thinking that would "force full GIPA
     * usage". The opposite is true: Wine's custom Vulkan bridge only calls
     * vk_icdGetInstanceProcAddr for 2 pre-instance functions regardless of
     * version, then resolves everything else through the VkInstance/VkDevice
     * dispatch tables. By restoring the real version we ensure the loader
     * picks the most capable path for instance and device dispatch.
     */
    typedef VkResult (VKAPI_CALL *PFN_Negotiate)(uint32_t*);
    PFN_Negotiate real_fn = (PFN_Negotiate)
        dlsym(g_real_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (real_fn) {
        uint32_t real_ver = *pSupportedVersion;
        LOGI("vk_icdNegotiateLoaderICDInterfaceVersion: calling real driver negotiate (ver in=%u)", real_ver);
        VkResult real_res = real_fn(&real_ver);
        LOGI("vk_icdNegotiateLoaderICDInterfaceVersion: real driver negotiate returned ver=%u res=%d", real_ver, real_res);
        /* Pass through — the loader gets the real driver's max version */
        *pSupportedVersion = real_ver;
    } else {
        LOGW("vk_icdNegotiateLoaderICDInterfaceVersion: real driver does NOT export negotiate, staying at loader_max=%u", loader_max);
        /* Leave *pSupportedVersion as loader_max for fallback compatibility */
    }

    LOGI("vk_icdNegotiateLoaderICDInterfaceVersion: RETURN version %u (PID=%d)",
         *pSupportedVersion, getpid());
    return VK_SUCCESS;
}

/* ========================================================================
 * vk_icdEnumerateAdapterPhysicalDevices — required for ICD interface v6+
 *
 * Forward to the real driver if it exports this function; fall back to
 * returning VK_ERROR_INCOMPATIBLE_DRIVER so the loader uses the ordinary
 * vkEnumeratePhysicalDevices path instead.
 * ======================================================================== */
__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdEnumerateAdapterPhysicalDevices(VkInstance instance,
                                      void      *adapter, /* LUID on Windows */
                                      uint32_t  *pPhysicalDeviceCount,
                                      VkPhysicalDevice *pPhysicalDevices)
{
    if (!ensure_real_icd()) return VK_ERROR_INCOMPATIBLE_DRIVER;

    typedef VkResult (VKAPI_CALL *PFN_EnumAdapterPD)(VkInstance, void*, uint32_t*, VkPhysicalDevice*);
    PFN_EnumAdapterPD real_fn = (PFN_EnumAdapterPD)
        dlsym(g_real_icd, "vk_icdEnumerateAdapterPhysicalDevices");
    if (real_fn) {
        LOGI("vk_icdEnumerateAdapterPhysicalDevices: forwarding to real driver");
        return real_fn(instance, adapter, pPhysicalDeviceCount, pPhysicalDevices);
    }
    LOGW("vk_icdEnumerateAdapterPhysicalDevices: real driver not found, returning incompatible");
    return VK_ERROR_INCOMPATIBLE_DRIVER;
}
