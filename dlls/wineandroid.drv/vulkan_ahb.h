/*
 * Wine Vulkan WSI for Android - AHardwareBuffer swapchain
 *
 * Copyright 2024 Winlator contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_VULKAN_AHB_H
#define __WINE_VULKAN_AHB_H

#include <stdint.h>
#include <stdbool.h>

/* Enable Android platform Vulkan extensions */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

/* IPC message types for Wine <-> Android app communication */
#define MSG_PRESENT  1
#define MSG_RELEASE  2
#define MSG_BUFFER   3
#define MSG_REQUEST  4

/* Maximum swapchain images */
#define AHB_MAX_IMAGES 4

/* IPC message structures */
struct present_msg {
    uint8_t  type;         /* MSG_PRESENT */
    uint32_t slot_index;
    int32_t  acquire_fd;   /* sent as SCM_RIGHTS ancillary data */
    int32_t  dst_x, dst_y, dst_w, dst_h;
};

struct release_msg {
    uint8_t  type;         /* MSG_RELEASE */
    uint32_t slot_index;
    int32_t  release_fd;   /* sent as SCM_RIGHTS ancillary data */
};

struct buffer_msg {
    uint8_t  type;         /* MSG_BUFFER */
    uint32_t slot_index;
    /* AHardwareBuffer handle sent via AHardwareBuffer_sendHandleToUnixSocket */
};

struct request_msg {
    uint8_t  type;         /* MSG_REQUEST */
    uint32_t slot_index;
};

/* Wine VkSurfaceKHR wrapper for AHB path */
struct wine_vk_surface {
    int  socket_fd;        /* Wine end of the Unix socket pair */
    int  width;            /* Surface dimensions */
    int  height;
    bool ahb_supported;    /* VK_ANDROID_external_memory_android_hardware_buffer available */
};

/* Wine VkSwapchainKHR wrapper for AHB path */
struct wine_vk_swapchain {
    struct wine_vk_surface *surface;
    VkDevice               device;
    uint32_t               image_count;
    uint32_t               current_image;

    struct {
        VkImage         vk_image;
        VkDeviceMemory  vk_memory;
        void           *ahb;          /* AHardwareBuffer* (opaque in Wine process) */
        int             slot_index;
        VkFence         reuse_fence;   /* Fence to wait on before reusing this image */
        int             release_fence; /* Pending release fd, -1 = none */
        bool            in_use;        /* Currently acquired by the application */
    } images[AHB_MAX_IMAGES];

    /* Vulkan function pointers (device-level) */
    PFN_vkGetFenceFdKHR          pfn_vkGetFenceFdKHR;
    PFN_vkImportFenceFdKHR       pfn_vkImportFenceFdKHR;
    PFN_vkCreateFence            pfn_vkCreateFence;
    PFN_vkDestroyFence           pfn_vkDestroyFence;
    PFN_vkWaitForFences          pfn_vkWaitForFences;
    PFN_vkResetFences            pfn_vkResetFences;
    PFN_vkAllocateMemory         pfn_vkAllocateMemory;
    PFN_vkFreeMemory             pfn_vkFreeMemory;
    PFN_vkCreateImage            pfn_vkCreateImage;
    PFN_vkDestroyImage           pfn_vkDestroyImage;
    PFN_vkBindImageMemory2       pfn_vkBindImageMemory2;
    PFN_vkGetDeviceProcAddr      pfn_vkGetDeviceProcAddr;
};

/*
 * Public API for the AHB Vulkan WSI
 */

/* Create a VkSurfaceKHR backed by an AHB socket connection.
 * socket_fd: the Wine end of the Unix socket pair to the Android app.
 * width, height: surface dimensions.
 * Returns the wine_vk_surface pointer cast to VkSurfaceKHR, or VK_NULL_HANDLE on failure.
 */
VkResult wine_ahb_create_surface(VkInstance instance, int socket_fd,
                                 int width, int height,
                                 VkSurfaceKHR *out_surface);

/* Destroy a surface created by wine_ahb_create_surface. */
void wine_ahb_destroy_surface(VkSurfaceKHR surface);

/* Check if the physical device supports AHB import.
 * Returns true if VK_ANDROID_external_memory_android_hardware_buffer is available.
 */
bool wine_ahb_check_device_support(VkPhysicalDevice phys_device,
                                   PFN_vkEnumerateDeviceExtensionProperties pfn_enumerate);

/* Surface capabilities for AHB surfaces */
VkResult wine_ahb_get_surface_capabilities(struct wine_vk_surface *surface,
                                           VkSurfaceCapabilitiesKHR *caps);

/* Surface formats for AHB surfaces */
VkResult wine_ahb_get_surface_formats(uint32_t *count, VkSurfaceFormatKHR *formats);

/* Create a swapchain backed by AHardwareBuffers */
VkResult wine_ahb_create_swapchain(VkDevice device, VkPhysicalDevice phys_device,
                                   struct wine_vk_surface *surface,
                                   const VkSwapchainCreateInfoKHR *create_info,
                                   struct wine_vk_swapchain **out_swapchain,
                                   PFN_vkGetDeviceProcAddr get_device_proc_addr,
                                   PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props);

/* Destroy a swapchain */
void wine_ahb_destroy_swapchain(struct wine_vk_swapchain *swapchain);

/* Acquire the next swapchain image */
VkResult wine_ahb_acquire_next_image(struct wine_vk_swapchain *swapchain,
                                     uint64_t timeout,
                                     VkSemaphore semaphore,
                                     VkFence fence,
                                     uint32_t *image_index);

/* Get swapchain images */
VkResult wine_ahb_get_swapchain_images(struct wine_vk_swapchain *swapchain,
                                       uint32_t *count, VkImage *images);

/* Present a rendered frame */
VkResult wine_ahb_queue_present(struct wine_vk_swapchain *swapchain,
                                VkQueue queue, uint32_t image_index,
                                VkFence render_fence);

/* Import an AHardwareBuffer into a swapchain slot as a VkImage */
VkResult import_ahb_to_vk_image(struct wine_vk_swapchain *swapchain,
                                uint32_t slot_index,
                                AHardwareBuffer *ahb,
                                VkPhysicalDevice phys_device,
                                PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props);

#endif /* __WINE_VULKAN_AHB_H */
