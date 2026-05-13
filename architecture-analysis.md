# Direct Android Compositing — Architecture Analysis

This document reflects the actual state of the codebase after the debugging
work in May 2026 that took the implementation from "Broforce only" to
"all tested Vulkan games run."

---

## Goal

Replace the X11-routed Vulkan present path:

```
DXVK → Wine X11 socket → Java X11 server → GL compositor → SurfaceFlinger
```

with a zero-copy direct compositing path:

```
DXVK → trojan swapchain → blit to AHardwareBuffer → SurfaceControl → SurfaceFlinger
```

OpenGL games (virgl, wined3d) keep using the X11 path unchanged.

---

## Three execution contexts

The pipeline spans three processes/contexts that must coordinate without
blocking each other:

**Wine process (Box64/ARM64)** — DXVK renders Vulkan frames. Our Vulkan
layer (`libahb_layer.so`) intercepts swapchain operations, creates a real
"trojan" swapchain to satisfy Wine's thunk validation, hands DXVK the
trojan's images to render into, and on every present blits trojan→AHB and
sends a present message to Android via a Unix socket. A dedicated
release-reader thread receives `MSG_RELEASE` messages and signals an
internal cv for waiters.

**Android app process** — Hosts the `AHardwareBufferPool`, a present-receiver
thread (`vulkan_jni.cpp`), a display thread (mailbox semantics), and the
`SurfaceControl` transaction submission. Sends `MSG_RELEASE` back to Wine
for both displayed frames (via `onComplete`) and dropped frames (via the
mailbox-drain in the receiver).

**SurfaceFlinger** — Receives the AHB directly via `ASurfaceTransaction_setBuffer`,
waits on the acquire `sync_fd` (the blit's completion fence), and scans
out. Fires the `onComplete` callback which drives the release chain.

---

## Approaches tried — what worked and what didn't

### Interception mechanism (3 attempts; only one survived)

| Approach | Outcome | Why |
|---|---|---|
| **LD_PRELOAD interceptor** (`ahb_preload.c`) | Abandoned | Worked for `vkCreateInstance`/`vkCreateDevice` via `RTLD_NEXT`, but Wine's `winevulkan` caches device-level function pointers in its own dispatch table, bypassing the `dlsym` chain. Couldn't reliably intercept `vkCreateSwapchainKHR` etc. for all games. |
| **ICD wrapper** (`ahb_icd_wrapper.c`) | Abandoned | Replaced `libvulkan_wrapper.so` entirely and patched the `VkInstance` dispatch at creation. Worked more reliably than LD_PRELOAD but still missed core functions cached by Wine's thunks. Higher risk of breaking other Vulkan users in the Wine process. |
| **Vulkan implicit layer** (`ahb_layer.c`) | **In production** | A proper Vulkan layer, loaded by the loader via `VK_INSTANCE_LAYERS=VK_LAYER_WINLATOR_ahb_direct`. Reliably intercepts all instance/device/swapchain extension functions. Core functions (e.g., `vkQueueSubmit`) still bypass us, but that turned out not to matter for the actual pipeline. |

The other two interceptors are now dead code. The `ahb_preload.c` and
`ahb_icd_wrapper.c` source files and their `.so` outputs can be deleted.
`GuestProgramLauncherComponent.java` still has a vestigial
`LD_PRELOAD += libahb_preload.so` conditional, but the `.so` is never
deployed, so it's a no-op.

### "Trojan" swapchain pattern

DXVK needs a valid `VkSwapchainKHR` handle that Vulkan validation accepts
and that Wine's thunks recognize. So `layer_CreateSwapchainKHR` creates a
**real** swapchain via the real driver (the "trojan") and returns its
handle to DXVK. But the trojan's images are never actually presented —
DXVK renders into them, we blit out to AHBs, and the trojan swapchain's
state machine is effectively ignored.

**What worked:** Returning the trojan handle satisfies DXVK and Wine.
DXVK renders into the trojan's `VkImage`s (which are real device-local
images bound to whatever surface Wine created), and we use those as the
source for our blit.

**What didn't:** An early version overrode the trojan's `presentMode` to
`MAILBOX` regardless of DXVK's request (with a fallback to `IMMEDIATE`),
on the theory that the trojan shouldn't block on vsync. This produced no
benefit — we never call `vkAcquireNextImageKHR` on the trojan — and the
theoretical concern was about a state difference between what DXVK
believed the swapchain to be (FIFO) and what the underlying driver had
(MAILBOX). After ruling out several other freeze causes we reverted to
honoring DXVK's requested mode and creating the trojan with whatever
DXVK asked for. No behavioral change observed.

### Blit-to-AHB indirection

The first version of `layer_QueuePresentKHR` was missing the actual blit —
the IPC plumbing was complete but no `vkCmdCopyImage` was recorded.
Adding `vkCmdCopyImage` made games run, but with **red and blue swapped**:
DXVK renders into a `B8G8R8A8_UNORM` image, and `vkCmdCopyImage` is a
byte-level memcpy. The AHB's native HAL format is `RGBA_8888`, so
SurfaceFlinger interpreted the bytes in the wrong channel order.

**The fix that worked:** Change the AHB-side `VkImage` import format to
`VK_FORMAT_R8G8B8A8_UNORM` (matching the AHB's native layout) and switch
the blit from `vkCmdCopyImage` to `vkCmdBlitImage`. `BlitImage` is
*format-aware* — it reads source pixels with BGRA semantics and writes
destination pixels with RGBA semantics, performing the channel swap as
a side effect of the format reinterpretation. Single-line change with a
clean Vulkan-spec basis.

### Per-slot blit resources and synchronization

Initially the blit used a single command buffer + fence reused across all
4 buffer slots. This caused Adreno driver timeouts at high frame rates
when the GPU was still working on the previous blit. Switched to
**per-slot command buffers and per-slot fences**, with each fence created
with `VkExportFenceCreateInfo` for `SYNC_FD_BIT` export. The blit submit
waits on DXVK's render-complete semaphores (forwarded from
`pPresentInfo->pWaitSemaphores`) so the copy doesn't start until DXVK's
writes are done. `wine_ahb_queue_present` exports the fence as a sync_fd
and ships it to Android via `SCM_RIGHTS`, where `ASurfaceTransaction_setBuffer`
takes ownership and gates SurfaceFlinger's scan-out on it.

### `VK_KHR_external_fence_fd` extension injection

Required for `vkGetFenceFdKHR` to succeed. DXVK doesn't always request
this extension on its own (it does for some games, not others), so the
layer injects it into the device's extension list inside `layer_CreateDevice`
if it isn't already present. Without this injection, every present
shipped to Android with `acquireFd=-1` (no sync_fd), which sometimes
worked by luck and sometimes caused tearing / cmd_buffer reuse races.

### The freeze at frame 3 (Vampire Survivors, Hollow Knight, GTA4)

For weeks, FIFO-mode games froze after a few frames while IMMEDIATE-mode
games (Broforce) worked. Theories that **did not** pan out:

- **Acquire semaphore wasn't signaled** — we signal it via an empty
  queue submit; verified working.
- **MAILBOX-override on the trojan confusing DXVK** — reverted; didn't
  change behavior.
- **FIFO-specific acquire-pacing missing** — added strict
  back-pressure that blocks acquire when `frames_in_flight >= image_count - 1`
  for FIFO games. Doesn't fix the freeze (the freeze happens at frame 3,
  before the pacing logic engages), but is correct behavior to keep.
- **Patching the dispatch table for `vkQueueSubmit`** — wanted to log
  every submit to see if DXVK was stuck before or after submission. The
  dispatch-table scan never finds `vkQueueSubmit` (Wine's `winevulkan`
  stores it in a different structure than our 512-entry scan reaches),
  so we have no layer-side visibility into core function calls.

**What actually found the bug:** Enabling DXVK's own log via
`DXVK_LOG_LEVEL=info` + `DXVK_LOG_PATH` (pointed at a Wine-writable
location inside the imagefs, since `/data/local/tmp` is blocked by
SELinux for app processes). The log revealed:

```
VK_KHR_present_wait
    presentWait                            : 1
```

DXVK had enabled `VK_KHR_present_wait` for FIFO swapchains and was
calling `vkWaitForPresentKHR(presentId=N, timeout=∞)` after every
present to wait for the frame to be observable on the display. Our
pipeline bypasses the real `vkQueuePresentKHR`, so the trojan
swapchain's presentId was never signaled, and DXVK waited forever.

**The fix:** Intercept `vkWaitForPresentKHR` in the layer and return
`VK_SUCCESS` immediately for the trojan swapchain. Semantically correct
— the present really did happen, just through the DAC path instead of
the real swapchain. All FIFO games then progressed past frame 3.

### Frame pacing — attempted real `vkWaitForPresentKHR` (reverted)

After getting games to run, motion jitter remained. The hypothesis: our
return-instant `vkWaitForPresentKHR` lets DXVK race ahead of vsync; if
we made it actually wait for SurfaceFlinger's `onComplete`, DXVK would
be paced to real display timing.

Two attempts, both unsuccessful:

**Attempt 1 — track `presentId` → slot via a ring buffer, mark the
newest matching record completed on each `MSG_RELEASE`, advance
`max_completed_present_id`, have `vkWaitForPresentKHR(N)` block on it.**
This caused "time-travel" — enemies moving backward briefly during
gameplay. Root cause: `MSG_RELEASE` doesn't carry a `presentId`, so the
"mark newest matching slot as the completed presentId" guess advanced
`max_completed_present_id` ahead of the actual displayed frame. DXVK
saw multiple waits return at once, submitted bursts of frames, mailbox
dropped them non-monotonically, and the game's dt-based physics
mis-extrapolated.

**Attempt 2 — extend the IPC protocol.** Added `present_id` to
`MSG_PRESENT` (so the Wine side passes DXVK's `VkPresentIdKHR` through)
and a `displayed` flag to `MSG_RELEASE` (1 for `onComplete`-driven
releases, 0 for mailbox-drain drops). The layer's release-reader thread
increments a `g_display_count` only on `displayed=1` releases. Required
synchronized rebuilds of both `libahb_layer.so` and `libwinlator.so`.
`vkWaitForPresentKHR` would then block until `display_count` advanced
one tick.

Result: framerate dropped to a hard 30 FPS, locked to a single slot
(slot 3 in every present). Why:

- DXVK's strict FIFO loop is **1-buffer-deep**: acquire → render →
  present → wait → repeat.
- SurfaceFlinger's `onComplete` fires ~1 vsync **after** apply (the
  buffer is "displayed" only after being latched at the next vsync).
- Each Wine iteration therefore takes ≥ 2 vsyncs: render + apply + 1
  vsync of latency.
- With 1-buffer cycling, only the most-recently-released slot is ever
  free, so Wine reuses the same slot indefinitely. The pipelining that
  FIFO normally allows (multiple frames in flight) never materializes.

The real fix would have been to use `ASurfaceTransaction_setOnCommit`
(API 31+, fires at apply rather than at display) to drive the
`display_count` tick, decoupling pacing from the slot-release chain.
This is a bigger Android-side change (new `MSG_TICK` message type,
display thread needs new callback wiring) and wasn't undertaken.

**Reverted** `vkWaitForPresentKHR` to instant-`VK_SUCCESS`. The IPC
extensions (`present_id` field, `displayed` flag) are kept in the
protocol because they're useful infrastructure for the future
`setOnCommit` work — they just go unused at the moment.

### Release-reader thread refactor (kept)

Previously `layer_AcquireNextImageKHR` read `MSG_RELEASE` from the
socket inline via `recv()`, both non-blocking (to drain) and blocking
(when slots were exhausted). Adding `vkWaitForPresentKHR` interception
would have created a race for the socket between two threads.

The refactor: a dedicated release-reader thread is the sole consumer of
the socket. It reads `MSG_RELEASE`, marks the slot free, optionally
bumps `g_display_count`, and signals a condition variable. Both
`layer_AcquireNextImageKHR` and (potentially) `layer_WaitForPresentKHR`
wait on that cv.

This is a strict architectural improvement (no socket contention,
slots freed eagerly the moment they're released) and is kept even
though the present-wait pacing wasn't ultimately enabled.

### 120 Hz panel rate request

Added `ASurfaceTransaction_setFrameRate(scanoutGameSC, 120.0f,
FRAME_RATE_COMPATIBILITY_DEFAULT)` in `VulkanRendererContext::initScanout`.
Whether this takes effect depends on the container's "Refresh Rate
Limit" setting in the Winlator UI — the Java Activity sets
`Window.preferredRefreshRate` from that container setting, and a value
of 60 (the default) overrides our 120 request at the Window level. To
get 120 Hz, the user must change the container's refresh-rate limit.

---

## Current implementation

### Component summary

```
dlls/vulkan_layer/ahb_layer.c       — Vulkan implicit layer (THE active interceptor)
dlls/vulkan_layer/ahb_layer.json    — layer manifest (loaded by Vulkan loader)
dlls/vulkan_layer/VkLayer_ahb_direct.json  — alt manifest path

dlls/wineandroid.drv/vulkan_ahb.c|h — Wine WSI: AHB import, present_msg IPC
dlls/wineandroid.drv/vulkan.c       — Wine driver entry points (mostly unused
                                       when layer is active; calls vulkan_ahb.c)

app/src/main/cpp/winlator/vulkan_jni.cpp        — Android present-receiver +
                                                   display thread (mailbox)
app/src/main/cpp/winlator/VulkanRendererContext.cpp  — SurfaceControl scanout,
                                                       blit→localAHB, onComplete
                                                       release callback
app/src/main/java/com/winlator/cmod/renderer/AHardwareBufferPool.java
                                                — AHB allocator + release-fence
                                                  background thread
app/src/main/java/com/winlator/cmod/xenvironment/components/DirectCompositorComponent.java
                                                — DAC lifecycle wrapper (init/
                                                  shutdown the pool + receiver)
app/src/main/java/com/winlator/cmod/renderer/VulkanRenderer.java
                                                — Java side renderer + HUD wiring
```

Dormant / dead code that's still in the tree:

```
dlls/vulkan_layer/ahb_preload.c          — LD_PRELOAD interceptor, never loaded
dlls/vulkan_layer/ahb_icd_wrapper.c      — ICD wrapper, never loaded
dlls/vulkan_layer/libahb_preload.*.so    — build artifacts, never deployed
dlls/vulkan_layer/libvulkan_wrapper_ahb.*.so  — build artifacts, never deployed
GuestProgramLauncherComponent.java:381-386    — adds libahb_preload to
                                                LD_PRELOAD if present (it's
                                                not), so this is a no-op
```

### IPC protocol (Wine ↔ Android, Unix socket, SCM_RIGHTS for fds)

```c
struct present_msg {
    uint8_t  type;          // MSG_PRESENT (1)
    uint32_t slot_index;    // 0..AHB_MAX_IMAGES-1
    int32_t  acquire_fd;    // blit completion sync_fd; -1 if none
    int32_t  dst_x, dst_y, dst_w, dst_h;
    uint64_t present_id;    // DXVK's VkPresentIdKHR; 0 if not provided
                            // (carried for future setOnCommit pacing)
};

struct release_msg {
    uint8_t  type;          // MSG_RELEASE (2)
    uint32_t slot_index;
    int32_t  release_fd;    // currently -1
    uint8_t  displayed;     // 1 = onComplete (actual vsync tick)
                            // 0 = mailbox drop (slot freed, not displayed)
                            // (carried for future setOnCommit pacing)
};
```

### Layer's intercepted functions

```
layer_CreateInstance / DestroyInstance          — chain through
layer_CreateDevice / DestroyDevice              — chain through; inject
                                                  VK_KHR_external_fence_fd;
                                                  patch dispatch table for
                                                  vkQueuePresentKHR + vkQueueSubmit
                                                  (sometimes fails but layer
                                                   chain still works for KHR funcs)
layer_GetPhysicalDeviceSurface*KHR              — surface query passthrough
layer_CreateSwapchainKHR                        — THE sniper hook: if request
                                                  dimensions match the AHB pool,
                                                  create a real "trojan" swapchain
                                                  with DXVK's exact mode, import
                                                  AHBs via vulkan_ahb.c, allocate
                                                  per-slot blit resources
layer_DestroySwapchainKHR                       — cleanup
layer_GetSwapchainImagesKHR                     — return trojan VkImages (which
                                                  DXVK renders into)
layer_AcquireNextImageKHR                       — pick a free slot from g_slot_free
                                                  (mailbox-style, blocking on cv
                                                  when saturated; FIFO games
                                                  enforce extra back-pressure)
layer_QueuePresentKHR                           — extract presentId from pNext,
                                                  blit trojan → AHB with channel
                                                  swap (BlitImage, format-aware),
                                                  submit blit on DXVK's queue
                                                  waiting on its render-complete
                                                  semaphore, send present_msg to
                                                  Android with sync_fd
layer_WaitForPresentKHR                         — returns VK_SUCCESS immediately
                                                  for trojan swap (proper pacing
                                                  would require setOnCommit work)
layer_QueueSubmit                               — diagnostic wrapper; logs and
                                                  forwards (dispatch-table patch
                                                  for this fails, so DXVK's
                                                  submits actually bypass us)
```

### Acquire-flow (mailbox-style with FIFO back-pressure)

```
pthread_mutex_lock(release_mtx)
while (need_block):
    cond_wait(release_cv)
    recount free_count
pick first-free slot not in g_presented_ring (recently-presented bias)
mark slot busy
pthread_mutex_unlock
signal DXVK's acquire semaphore/fence via empty QueueSubmit on g_saved_queue

need_block =
    free_count == 0
    OR (mode is FIFO AND s_acquire_counter > image_count
        AND frames_in_flight >= image_count - 1)
```

### Present-flow

```
extract present_id from VkPresentIdKHR pNext
wait/reset per-slot fence (slot N's previous blit must be done; release
                           chain guarantees this — when slot N is reusable,
                           SurfaceFlinger already consumed it)
record blit command buffer:
  barrier  trojan: PRESENT_SRC_KHR  → TRANSFER_SRC_OPTIMAL
  barrier  AHB:    UNDEFINED        → TRANSFER_DST_OPTIMAL
  vkCmdBlitImage trojan → AHB  (format-aware, R↔B handled implicitly)
  barrier  trojan: TRANSFER_SRC_OPTIMAL → PRESENT_SRC_KHR (restore for DXVK)
  barrier  AHB:    TRANSFER_DST_OPTIMAL → GENERAL          (for SF handoff)
submit blit waiting on pPresentInfo->pWaitSemaphores, signaling fence
update g_presented_ring (history of last 2 slots, used by acquire bias)
wine_ahb_queue_present(swap, queue, slot, fence, presentId)
    → exports fence as SYNC_FD via vkGetFenceFdKHR
    → sends present_msg over socket with sync_fd as SCM_RIGHTS
```

### Release-reader thread (in `connect_ahb`)

```
loop:
    recv(socket, &release_msg)
    if MSG_RELEASE:
        lock release_mtx
        g_slot_free[slot] = true
        if displayed: g_display_count++   (unused while WaitForPresent is no-op)
        broadcast release_cv
        unlock release_mtx
```

### Android-side present receiver (`vulkan_jni.cpp`)

```
loop:
    recvmsg(socket, &present_msg, SCM_RIGHTS)
    extract acquireFd from ancillary data
    mailbox-drain: while more data pending:
        recv next present_msg
        send MSG_RELEASE for old slot with displayed=0
        replace current with new
    push latest slot to mailboxSlot (atomic exchange)
```

### Display thread

```
loop:
    slot = mailboxSlot.exchange(-1)
    if slot < 0: usleep(1ms); continue
    if !scanoutInitialized: initScanout()  (lazy: SC layers + 120Hz request)
    scanoutSetBuffer(ahb, acquireFd, slot)
    applyScanoutBuffer():
        ASurfaceTransaction_setBuffer(scanoutGameSC, ahb, acquireFd)
        ASurfaceTransaction_setGeometry(...)
        ASurfaceTransaction_setVisibility(SC, 1)
        ASurfaceTransaction_setOnComplete(transaction, [](){
            send MSG_RELEASE for prev_displayed_slot with displayed=1
        })
        ASurfaceTransaction_apply()
```

---

## Status: which games work

Tested on Odin 2 Portal, Android 13, Adreno 740, Proton 9.0, DXVK 2.x:

| Game | Mode | Status |
|---|---|---|
| Broforce | IMMEDIATE | ✅ runs, 60 FPS, some motion jitter |
| Vampire Survivors | FIFO | ✅ runs (after present_wait fix), some jitter |
| Hollow Knight | FIFO | ✅ runs (after present_wait fix) |
| GTA4 | FIFO | ✅ runs (after present_wait fix) |
| Megabonk | FIFO | ✅ runs (after present_wait fix) |

---

## Known issues / future work

1. **Frame pacing jitter.** Visible motion irregularity especially during
   character movement. Root cause: DXVK renders at variable speed (15–30
   ms per frame from box64 + shader compile overhead), `vkWaitForPresentKHR`
   returns instantly so DXVK doesn't pace itself, and the DAC path has
   no compositor-side smoothing (which the X11 path provided as a side
   effect of its extra latency). The architecturally correct fix is to
   drive `g_display_count` from `ASurfaceTransaction_setOnCommit` (API
   31+) and have `vkWaitForPresentKHR` wait on it. Requires:
   - New `MSG_TICK` IPC message type
   - Display thread registers `setOnCommit` callback that sends `MSG_TICK`
   - Release-reader thread treats `MSG_TICK` as the pacing signal
   - `layer_WaitForPresentKHR` waits on `g_display_count`

2. **HUD FPS counter shows 0 under DAC.** `directFrameCount` in
   `VulkanRendererContext` ticks per frame, but `WinlatorHUD.onFrame()`
   is only called from the X server's window-update path which is
   disabled once DAC takes over. Fix: add a periodic Java poller that
   reads the native counter and calls `hud.onFrame()` for each delta.

3. **Dynamic AHB pool size.** The sniper hook only matches the AHB
   pool's fixed 1280×720 dimensions. Games that switch resolution
   mid-run fall back to passthrough (X11 path). Requires a new IPC
   message asking Android to reallocate AHBs at the new size, plus
   coordinated tear-down/rebuild of the layer's per-slot blit resources.

4. **Dispatch table patching is unreliable.** Wine's `winevulkan`
   stores device function pointers in a structure our 512-entry linear
   scan doesn't reach, so the patch for `vkQueuePresentKHR` and
   `vkQueueSubmit` always fails. The layer chain still intercepts KHR
   extension functions (because they go through `vkGetDeviceProcAddr`),
   but core functions like `vkQueueSubmit` are not intercepted. Not
   currently blocking anything but limited diagnostic visibility.

5. **Dead code in tree.** `ahb_preload.c`, `ahb_icd_wrapper.c`, their
   build outputs, and the conditional `LD_PRELOAD` injection in
   `GuestProgramLauncherComponent.java` all reference an interception
   path we no longer use. Safe to delete.

6. **`VK_EXT_swapchain_maintenance1` and `VK_GOOGLE_display_timing`.**
   DXVK enables these too (per the device-extensions list in its log).
   If any DXVK code path calls `vkReleaseSwapchainImagesEXT` or
   `vkGetPastPresentationTimingGOOGLE` on our trojan, similar
   bypass-related hangs are possible. None observed yet; defensive
   interception would follow the same pattern as `vkWaitForPresentKHR`
   (return success / dummy data).

---

## Key lesson

When a Vulkan layer replaces the implementation of a swapchain extension
function (here `vkQueuePresentKHR`), it must also handle every dependent
function that the extension contract introduces. `VK_KHR_present_wait`
defines `vkWaitForPresentKHR`; replacing present without also handling
the wait broke FIFO games for weeks. The same pattern will recur for any
future extension that creates an async "the present reached the display"
expectation.

The diagnostic that finally exposed this was DXVK's own `info`-level log,
which listed the enabled extensions. Without it we'd still be guessing.
For any future hang of this shape, enable `DXVK_LOG_LEVEL=info` first.
