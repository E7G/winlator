# Direct Android Compositing — Architecture Analysis

## What This Feature Is

The goal is to eliminate the existing frame delivery chain for Vulkan-based games (DXVK / VKD3D):

```
DXVK → Wine X11 socket → Java X11 server → GL compositor → SurfaceFlinger
```

And replace it with a zero-copy path:

```
DXVK → AHardwareBuffer (GPU memory) → SurfaceControl → SurfaceFlinger
```

OpenGL games (virgl, wined3d) continue using the X server path unchanged. The X server is preserved intact throughout Phase 1 and can be deleted in a future phase.

---

## The Three-Layer Architecture

The system spans three distinct execution contexts that must coordinate without blocking each other.

### Layer 1 — Wine Process (Box64 / ARM64)

DXVK or VKD3D renders into `VkImage` objects that are backed by `AHardwareBuffer` memory. These buffers are owned by the Android app but imported into Vulkan on the Wine side. When a frame is ready, Wine exports a GPU fence as a sync fd (acquire fence) and sends it to the Android app over a Unix socket. Wine then waits for a release fence to come back before reusing that image slot.

### Layer 2 — Android App Process

Manages the `AHardwareBufferPool`, the `SurfaceControl` layers, and the `ASurfaceTransaction` submission loop. When it receives a present message from Wine, it calls `ASurfaceTransaction_setBuffer` with the buffer pointer and the acquire fence. SurfaceFlinger takes it from there. When SurfaceFlinger is done with the buffer, the `onComplete` callback fires and the release fence is forwarded back to Wine.

### Layer 3 — SurfaceFlinger

Receives the buffer directly from GPU memory. Waits on the acquire fence before scan-out. Signals the release fence when done. No pixel copy occurs at any point.

---

## Component Breakdown

### `AHardwareBufferPool.java` ✅ Complete

Pre-allocates N buffers (default 3) at startup. Provides `acquire()` / `release(index, fd)` semantics with a 16ms timeout (one frame at 60Hz). Release fences are waited on by a background daemon thread pool so the Wine presentation thread is never blocked on a fence signal. The pool is the single source of truth for buffer ownership.

Key design choices:
- Buffers are allocated with `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` and `GPU_FRAMEBUFFER | GPU_SAMPLED_IMAGE | COMPOSER_OVERLAY` usage flags.
- The `AHardwareBufferNativeCalls` interface is injected, making the class fully testable without loading `libwinlator.so`. This enabled the comprehensive jqwik property-based test suite.
- Consecutive drop count is tracked and logged so frame budget overruns are visible in logcat.

### `ahb_bridge.c` (JNI, `libwinlator.so`) ✅ Complete

Five JNI functions that wrap NDK calls:
- `nativeCreateBuffer` / `nativeDestroyBuffer` — `AHardwareBuffer_allocate` / `_release`
- `nativeSendBufferToSocket` / `nativeReceiveBufferFromSocket` — `AHardwareBuffer_sendHandleToUnixSocket` / `_recvHandleFromUnixSocket`
- `nativeWaitFence` — `sync_wait()` from `<sync/sync.h>`, closes the fd regardless of outcome

All functions return sentinel values (0 for pointers, -1 for fds) on failure and log with tag `"AHB_Bridge"`. The `sync` library was added to `target_link_libraries` in `CMakeLists.txt`.

### `vulkan_ahb.c` / `vulkan_ahb.h` (Wine WSI) ✅ Complete

Implements the Vulkan WSI inside the Wine process. The key structures are `wine_vk_surface` (holds the socket fd and dimensions) and `wine_vk_swapchain` (holds the per-slot `VkImage`, `VkDeviceMemory`, `AHardwareBuffer*`, reuse fence, and release fence).

The IPC protocol uses four fixed-size message types over a Unix socket with `SCM_RIGHTS` ancillary data for fd passing:
- `MSG_BUFFER` (3): Android → Wine, sends an AHB handle at swapchain creation
- `MSG_REQUEST` (4): Wine → Android, requests a buffer slot
- `MSG_PRESENT` (1): Wine → Android, sends slot index + acquire fence fd
- `MSG_RELEASE` (2): Android → Wine, sends slot index + release fence fd

Surface capabilities report `minImageCount=2`, `maxImageCount=3`, and exactly one format: `VK_FORMAT_R8G8B8A8_UNORM / VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. If `VK_ANDROID_external_memory_android_hardware_buffer` is absent, the surface returns `VK_ERROR_SURFACE_LOST_KHR` and Wine falls back to the X11 path.

A deliberate design decision: AHBs are **not** released in `wine_ahb_destroy_swapchain`. The Android-side pool owns the buffer lifetime. Wine only holds a reference via `recvHandleFromUnixSocket`. Releasing on the Wine side would decrement the refcount while the Android present-receiver thread might still be using the buffer.

### `ahb_preload.c` (LD_PRELOAD interceptor) ✅ Complete

Loaded via `LD_PRELOAD` before any other library. Intercepts `vkCreateDevice`, `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR`, `vkGetSwapchainImagesKHR`, `vkQueuePresentKHR`, and `vkGetDeviceProcAddr` at the dynamic linker level using `RTLD_NEXT`.

Connection to the AHB server is deferred to the first `vkCreateDevice` call. This is intentional: Wine helper processes (wineserver, services.exe, etc.) never call `vkCreateDevice`, so they never connect to the socket and never disrupt the present receiver thread.

On connection, the interceptor pre-receives 3 `AHardwareBuffer` handles from the Android app. At swapchain creation time, it imports these directly into Vulkan via `import_ahb_to_vk_image` rather than going through the socket request/receive cycle again.

The dispatch table patching in `vkCreateDevice` is the most fragile part: it scans the first 32 entries of the VkDevice dispatch table looking for the real `vkGetDeviceProcAddr` pointer and replaces it with the wrapper. This ensures DXVK's internal `vkGetDeviceProcAddr` calls still hit the interceptor even when Wine resolves functions through the device vtable rather than through `LD_PRELOAD`.

### `ahb_icd_wrapper.c` (ICD wrapper) ✅ Complete

A complementary strategy that replaces `libvulkan_wrapper.so` entirely. Exports `vk_icdGetInstanceProcAddr` and patches the VkInstance dispatch table at creation time. This is the more robust primary path; the LD_PRELOAD interceptor is the fallback for cases where dispatch table patching fails.

### `VulkanRendererContext.cpp` (Android-side C++) ⚠️ Partially Complete

The core infrastructure is present:
- `scanoutSetBuffer` signature extended to accept `int acquireFenceFd`
- `pollReleaseFence()` method added to drain the release fence queue
- `directFrameCount` atomic counter added
- `releaseQueue` ring buffer with `releaseMutex` for async release fence delivery

What's incomplete: the `ASurfaceTransaction_setOnComplete` callback registration needs verification, and the Java-side polling of `nativePollReleaseFence()` after each `nativeScanoutSetBuffer` call needs to be wired up.

### `DirectCompositorComponent.java` ✅ Complete (per tasks)

The `EnvironmentComponent` wrapper that owns the pool lifecycle. Critically, it has zero imports from `com.winlator.cmod.xserver.*`, which is the Phase 4 deletion readiness requirement. `start()` checks API level ≥ 26, allocates the pool, calls `pool.init()`, and activates `nativeMode` on the renderer. `stop()` reverses this in order.

### `VulkanRenderer.java` ✅ Complete (per tasks)

`setGraphicsDriver()` activates `nativeMode` for `"dxvk"` and `"vkd3d"` drivers (case-insensitive) and guards against API < 26. The first-frame transition (`!wasDelivered && delivered`) logs the observability message and calls `xServer.setRenderingEnabled(false)`. The HUD wiring via `hudRef.setIsNative(boolean)` was already in place.

---

## IPC Protocol Detail

```
Wine Process                          Android App
─────────────────────────────────────────────────────────────────

[swapchain creation]
  ← MSG_BUFFER{slot=0, AHB handle via SCM_RIGHTS}
  ← MSG_BUFFER{slot=1, AHB handle via SCM_RIGHTS}
  ← MSG_BUFFER{slot=2, AHB handle via SCM_RIGHTS}

[each frame]
  vkQueuePresentKHR(slot)
  vkGetFenceFdKHR → acquire_fd
  → MSG_PRESENT{slot, acquire_fd via SCM_RIGHTS, dst_x,y,w,h}

                                      recv MSG_PRESENT
                                      ASurfaceTransaction_setBuffer(ahb, acquire_fd)
                                      ASurfaceTransaction_apply()
                                      [SurfaceFlinger scans out]
                                      onComplete callback → release_fd
                                      ← MSG_RELEASE{slot, release_fd via SCM_RIGHTS}

  recv MSG_RELEASE
  vkImportFenceFdKHR(reuse_fence, release_fd)
  [next frame] vkWaitForFences(reuse_fence)
  vkQueueSubmit(render into slot)
```

File descriptors are passed as `SCM_RIGHTS` ancillary data in `sendmsg`/`recvmsg`. The acquire fence ownership transfers to the NDK (`ASurfaceTransaction_setBuffer` takes it). The release fence ownership transfers to the pool's background fence-waiter thread, which closes it after `sync_wait`.

---

## Fence Ownership Table

| Fence | Created by | Consumed by | Closed by |
|-------|-----------|-------------|-----------|
| Acquire fd | Wine (`vkGetFenceFdKHR`) | NDK `ASurfaceTransaction_setBuffer` | NDK (takes ownership) |
| Release fd | SurfaceFlinger (`onComplete`) | `pool.release(index, fd)` | Pool background thread after `sync_wait` |
| Reuse VkFence | Wine WSI | `vkWaitForFences` before next render | Wine WSI at swapchain destroy |

---

## Challenges and Walls Hit

### 1. Vulkan Dispatch Table Patching

**The problem**: Wine's Vulkan bridge calls `vk_icdGetInstanceProcAddr` only twice (for pre-instance functions), then resolves all subsequent functions through the VkInstance/VkDevice dispatch tables. This completely bypasses `LD_PRELOAD` for device-level calls. DXVK does the same — it caches function pointers at device creation time via `vkGetDeviceProcAddr`, so even if `LD_PRELOAD` intercepts the initial lookup, subsequent calls go directly through the cached pointer.

**The solution**: Both interceptors patch the dispatch table in-place at `vkCreateDevice` time. The LD_PRELOAD interceptor scans the first 32 entries of the VkDevice dispatch table looking for the real `vkGetDeviceProcAddr` pointer and replaces it with the wrapper. The ICD wrapper patches the VkInstance dispatch table at creation time.

**Remaining risk**: The scan-and-replace approach is fragile. If the dispatch table layout changes between Vulkan loader versions, or if the real function pointer appears at index > 32, the patch silently fails. The log message `"could not find vkGetDeviceProcAddr in dispatch table"` is the only signal. This is why both strategies coexist — if one fails, the other may succeed.

### 2. Process Filtering (Wine Helper Processes)

**The problem**: Wine spawns multiple helper processes (wineserver, services.exe, explorer.exe, etc.) that all inherit the `LD_PRELOAD` and `ANDROID_AHB_SERVER` environment variables. If any of them connect to the AHB socket, they disrupt the present receiver thread on the Android side, which expects exactly one connection from the game process.

**The solution**: Connection is deferred to the first `vkCreateDevice` call. Helper processes never call `vkCreateDevice`, so they never connect. The process cmdline is logged on startup to make it visible which processes are loading the interceptor.

**Remaining concern**: If a helper process somehow calls `vkCreateDevice` (e.g., a future Wine version adds GPU-accelerated compositing to explorer.exe), this filter breaks. A more robust approach would be to filter by process name or use a connection handshake that includes the process role.

### 3. Release Fence Delivery Across Process Boundaries

**The problem**: `ASurfaceTransaction_setOnComplete` fires on SurfaceFlinger's display thread inside the Android app process. The release fence fd must travel from that callback, through the Android app's JNI layer, across the Unix socket, into the Wine process, and be imported into Vulkan — all without blocking the Wine presentation thread or the SurfaceFlinger display thread.

**The solution**: A non-blocking ring buffer (`releaseQueue`) in `VulkanRendererContext` is populated by the `onComplete` callback. The Java side polls `nativePollReleaseFence()` after each `nativeScanoutSetBuffer` call and forwards results to `pool.release(index, fd)`. The pool's background thread then waits on the fence without blocking anything critical.

**Incomplete**: The wiring of `nativePollReleaseFence()` in `VulkanRenderer.java` after each `nativeScanoutSetBuffer` call needs verification. If this polling is not happening, release fences are never delivered back to Wine, the pool never marks slots as available, and `acquire()` will time out after 3 frames.

### 4. AHB Ownership Across Swapchain Recreation

**The problem**: DXVK destroys and recreates swapchains frequently — on window resize, fullscreen toggle, driver reset, and sometimes just during initialization. Each recreation would normally require new AHBs, but the pool has a fixed set. If Wine releases the AHB references on swapchain destroy, the pool's buffers become invalid.

**The solution**: `wine_ahb_destroy_swapchain` explicitly does **not** call `AHardwareBuffer_release`. The comment in the code explains this: the AHB is owned by the Android-side pool. Wine received a handle via `recvHandleFromUnixSocket` which gives a reference, but releasing it would decrement the refcount and potentially invalidate the buffer while the Android present-receiver thread is still using it. The pool manages the full lifetime.

**Implication**: On swapchain recreation, Wine must re-request the same AHB handles from the Android app via the socket. The pool sends the same buffer pointers (they haven't changed), and Wine re-imports them into new `VkImage` objects.

### 5. API Level Fragmentation

**The problem**: The feature requires three different API levels:
- `AHardwareBuffer_allocate`: API 26+
- `SurfaceControl` sibling layers: API 29+
- `ASurfaceTransaction_setOnComplete`: API 29+
- `ASurfaceTransaction_setOnCommit` (lower latency): API 31+

The app's `minSdkVersion` is lower than 26, so none of these can be called unconditionally.

**The solution**: All API 26+ code is guarded with `Build.VERSION.SDK_INT >= 26` checks. `ASurfaceTransaction` symbols are loaded dynamically via `dlopen`/`dlsym` in `VulkanRendererContext::loadScanoutApi()` — this was already in place before this feature. The NDK target API for `libwinlator.so` and `libvulkan_renderer.so` is set to 26. If the device is below API 26, `nativeMode` is permanently disabled with a one-time warning.

### 6. No-Allocation Hot Path

**The requirement**: No `new` in Java and no `malloc` in C on the path from `vkQueuePresentKHR` to `ASurfaceTransaction_apply`.

**The approach**: The pool pre-allocates everything at `init()` time. The `releaseQueue` is a bounded vector pre-sized to the pool count. The fence-waiter thread pool is a cached executor (threads are reused, not created per release). The socket messages are fixed-size structs on the stack.

**The gap**: The `fenceWaiter.submit(lambda)` call in `AHardwareBufferPool.release()` allocates a `Runnable` lambda object on the Java heap. This is technically an allocation on the release path, though not on the acquire/present hot path. Whether this matters in practice depends on GC pressure.

### 7. Two Interceptor Strategies Coexisting

**The problem**: It's not clear which interception strategy will work reliably across all Wine/DXVK/driver combinations. The LD_PRELOAD approach is simpler but can be bypassed by dispatch table caching. The ICD wrapper approach is more robust but requires replacing `libvulkan_wrapper.so` entirely, which may break other Vulkan users in the Wine process.

**The current state**: Both strategies are implemented and coexist. The ICD wrapper (`ahb_icd_wrapper.c`) is the primary path; the LD_PRELOAD interceptor (`ahb_preload.c`) is the fallback. This adds complexity — there are now two code paths that must be kept in sync, and it's not always clear which one is active at runtime.

**The unresolved question**: In production, which one actually intercepts DXVK's swapchain calls? The diagnostic logging (process cmdline, GIPA call log, swapchain creation log) is designed to answer this, but it hasn't been exercised end-to-end yet.

---

## What's Complete vs. What's Not

### Fully Implemented
- `AHardwareBufferPool.java` with property-based tests (jqwik)
- `ahb_bridge.c` JNI bridge
- `vulkan_ahb.c` / `vulkan_ahb.h` Wine WSI
- `ahb_preload.c` LD_PRELOAD interceptor
- `ahb_icd_wrapper.c` ICD wrapper
- `DirectCompositorComponent.java` lifecycle wrapper
- `VulkanRenderer.java` path selection and observability logging
- Build system changes (`CMakeLists.txt`, `sync` library link)
- Integration wiring into `XEnvironment` / `GuestProgramLauncherComponent`

### Partially Implemented
- `VulkanRendererContext.cpp`: acquire fence parameter and release fence queue are present, but the `ASurfaceTransaction_setOnComplete` callback registration and the Java-side `nativePollReleaseFence()` polling loop need verification
- Several property tests marked `*` (optional) are not yet written: Properties 4, 5, 7, 8, 9, 10, 11, 12, 14

### Not Yet Exercised End-to-End
- The full frame delivery pipeline (Wine WSI → AHB pool → SurfaceControl → SurfaceFlinger) has not been run on a real device
- Cursor compositing on the direct path
- Pause/resume lifecycle handling
- Surface reconstruction after `onSurfaceDestroyed`/`onSurfaceCreated`

---

## Phase 4 Deletion Readiness

A key architectural constraint is that the X server must be deletable in a future phase without touching the new components. This is enforced by:

- `DirectCompositorComponent.java` has zero imports from `com.winlator.cmod.xserver.*`
- `AHardwareBufferPool.java` has zero imports from `com.winlator.cmod.xserver.*`
- `ahb_bridge.c` has no dependency on X server headers
- The path selection in `VulkanRenderer` is a simple string comparison on `graphicsDriver`, not a structural dependency on the X server

When Phase 4 arrives, deleting `XServerComponent`, `GLRenderer`, `VirGLRendererComponent`, and the `com.winlator.cmod.xserver.*` package should not require changes to any of the new components.

---

## Observability

Log tags and key messages:

| Tag | Message | Meaning |
|-----|---------|---------|
| `VulkanRenderer` | `nativeMode enabled, direct compositing path active` | Path activated for this session |
| `VulkanRenderer` | `first scanout frame delivered` | First direct frame reached SurfaceFlinger |
| `AHB_Pool` | `acquire: timeout after 16 ms, consecutiveDrops=N` | Frame budget exceeded, N frames dropped |
| `AHB_ICD` | `vkCreateSwapchainKHR: AHB SWAPCHAIN CREATED!` | ICD wrapper intercepted swapchain creation |
| `AHB_Preload` | `vkCreateSwapchainKHR: AHB SWAPCHAIN CREATED!` | LD_PRELOAD interceptor intercepted swapchain creation |
| `Wine_AHB_WSI` | `vkQueuePresentKHR: frame N presented` | Frame submitted from Wine side |
| `AHB_Bridge` | (any error) | JNI bridge NDK call failed |

The HUD indicator (`hudRef.setIsNative(boolean)`) shows per-frame whether the direct path or the X server path delivered the frame.

---

## Summary

The architecture is sound and the core components are well-designed. The zero-copy guarantee is real — no `AHardwareBuffer_lock` is called on the hot path, and `ASurfaceTransaction_setBuffer` passes the buffer directly to SurfaceFlinger. The fence synchronization model correctly handles the three-way coordination between GPU rendering (Wine), display scan-out (SurfaceFlinger), and buffer reuse (Wine again).

The main open questions are operational rather than architectural:

1. Does the dispatch table patching reliably intercept DXVK's swapchain calls in practice?
2. Is the `nativePollReleaseFence()` polling loop actually wired up and draining the release queue?
3. Does the full pipeline work end-to-end on a real device with a real DXVK game?

These can only be answered by running it.
