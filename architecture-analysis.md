# Direct Android Compositing — Architecture Analysis

This document reflects the state of the codebase after the May 2026
debugging series. The pipeline now supports **three** user-selectable modes
exposed via a per-container / per-shortcut **Graphics Pipeline** dropdown:

- **Quality (direct-render)** — DXVK renders straight into AHB-backed
  `VkImage`s. No intermediate copy. Default for new containers.
- **Performance (trojan-blit)** — DXVK renders into a device-local trojan
  image and the layer blits it to the AHB on present.
- **Native (X11)** — DAC layer disabled via `DISABLE_AHB_LAYER=1`; game
  runs through the original `winex11` path. Compatibility fallback.

The choice is wired through `Container.graphicsPipeline` (per-container
default) and an optional per-shortcut override stored in the shortcut's
`graphicsPipeline=` extra. `GuestProgramLauncherComponent` resolves the
value and re-asserts the relevant env vars **after** the shortcut's
`envVars=` merge so the spinner is the final authority (legacy
hand-written `WINLATOR_AHB_DIRECT_RENDER=...` lines no longer pin the
mode).

---

## Goal

Replace the X11-routed Vulkan present path:

```
DXVK → Wine X11 socket → Java X11 server → GL compositor → SurfaceFlinger
```

with a zero-copy direct compositing path. Two flavours coexist:

```
Quality:      DXVK → AHB-backed VkImage  →  SurfaceControl → SurfaceFlinger
                    (zero-copy, fence-only submit signals SYNC_FD)
Performance:  DXVK → trojan VkImage → vkCmdBlitImage → AHB → SurfaceControl → SF
                    (one blit, more driver-friendly source image)
```

OpenGL games (virgl, wined3d) and Native-pipeline Vulkan games keep using
the X11 path unchanged.

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

### Blit-to-AHB indirection (Performance mode)

The first version of `layer_QueuePresentKHR` was missing the actual blit —
the IPC plumbing was complete but no `vkCmdCopyImage` was recorded.
Adding `vkCmdCopyImage` made games run, but with **red and blue swapped**:
DXVK renders into a `B8G8R8A8_UNORM` image, and `vkCmdCopyImage` is a
byte-level memcpy. The AHB's native HAL format was originally `RGBA_8888`,
so SurfaceFlinger interpreted the bytes in the wrong channel order.

**Two successive fixes, both in production now:**

1. Use `vkCmdBlitImage` (format-aware) instead of `vkCmdCopyImage`. Reading
   the source as BGRA and writing the destination as RGBA performs the
   channel swap implicitly via format reinterpretation.
2. **Unify the AHB pool to `HAL_PIXEL_FORMAT_BGRA_8888`** and import the
   AHB-side `VkImage` as `VK_FORMAT_B8G8R8A8_UNORM` to match. Both ends
   of the blit are now the same format, so the operation degenerates to
   a byte copy — but the byte layout matches what SurfaceFlinger expects
   when reading a HAL_BGRA buffer. No channel reinterpretation needed.

The unified BGRA pool is what direct-render mode also requires: DXVK
writes BGRA bytes directly into the AHB, the AHB is HAL_BGRA, and the
receiver displays it with no per-frame format-aware blit. The
`bgra_bytes` flag in `present_msg` (always 1 after unification) tells
the receiver to treat the buffer as BGRA-ordered bytes.

### Direct-render mode (Path 3)

In addition to trojan-blit, the layer now supports a **direct-render** mode
(`WINLATOR_AHB_DIRECT_RENDER=1`, the default since Container's
`DEFAULT_GRAPHICS_PIPELINE = "quality"`). Here the AHB-backed `VkImage`s
are returned to DXVK directly from `vkGetSwapchainImagesKHR` — DXVK
renders into them with no intermediate buffer.

**No trojan swapchain is created at all** in this mode. The "synthetic"
`VkSwapchainKHR` handle returned to DXVK is just a pointer to the
layer's own `wine_vk_swapchain` struct. DXVK's swapchain operations
(`Acquire`, `QueuePresent`, `WaitForPresent`, `Destroy`) are all
intercepted by the layer's `g_real_swapchain_handle` check; the real
driver never sees them.

**The present-side work** reduces to: reset a per-slot
SYNC_FD-exportable fence, submit an *empty* command buffer that waits
on DXVK's render-complete semaphores and signals the fence, then ship
the fence as a SYNC_FD via `wine_ahb_queue_present`. No barriers, no
blit, ~1 ms of GPU work saved per frame plus no trojan-image memory
overhead at swapchain creation.

Trojan-blit is kept as a Performance-mode option because:
- Some games with restrictive `imageUsage` flags on their swapchain
  request can't be served directly from AHB (which we always create
  with `COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST`).
- On certain SoC/driver combos, the AHB-as-render-target path has a
  slightly higher per-frame cost than trojan-blit, where the trojan is
  a regular device-local image and the blit is a single same-format
  copy.

The two modes share all infrastructure except the `QueuePresent` work
and the `GetSwapchainImagesKHR` return path. They live behind a single
`g_direct_render_mode` boolean set from the env var at device creation.

### FIFO image-count cap (trojan-blit only)

The AHB pool has 4 slots, but the real (trojan) swapchain returns
whatever the driver chooses — typically **2-3 for FIFO** present mode,
**3-4 for MAILBOX/IMMEDIATE**. Before this fix, `layer_AcquireNextImageKHR`
could return an `ahb_slot ≥ g_trojan_image_count` where the per-slot
`g_copy_cmd_bufs[slot]` and `g_copy_fences[slot]` were `VK_NULL_HANDLE`.
The blit branch in `QueuePresent` was then skipped, falling through to
the fallback that forwards the AHB to the receiver **without doing the
trojan→AHB copy**. The receiver displayed stale bytes — game appeared
frozen on the first frame that landed in an orphan slot.

This matched Vampire Survivors' (FIFO) freeze-at-start signature while
Broforce (IMMEDIATE) ran fine. Direct-render mode is unaffected
because there's no trojan and no per-slot blit infrastructure.

**Fix:** After creating the trojan and resolving its image count, cap
`sc->image_count = min(sc->image_count, g_trojan_image_count)`. Unused
imported AHB slots remain in memory (cheap) and the `wine_ahb_destroy_swapchain`
loop was widened to iterate `AHB_MAX_IMAGES` with NULL checks so the
extra slots get freed at swapchain teardown.

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

### Frame pacing — current state: phase-anchored absolute sleep

`layer_WaitForPresentKHR` is no longer a no-op. After two failed
release-chain-driven attempts (documented below), the current
implementation uses a self-contained pacing primitive that doesn't
depend on the IPC release chain or DXVK's render time at all.

**Current implementation:**

- `layer_WaitForPresentKHR` is intercepted for any swapchain matching
  `g_real_swapchain_handle` (true for both direct-render and trojan-blit
  modes).
- On each call it computes an absolute target wake time:
  - **Phase-anchored path**: `target = g_last_hardware_vsync_us +
    N × 16667` where N is the smallest integer making `target` lie in
    the future. The anchor `g_last_hardware_vsync_us` is updated by the
    release-reader thread whenever Android delivers an `MSG_VSYNC`
    message (driven by an `AChoreographer` callback on the Java side).
    Live as long as a vsync arrived in the last 3 periods.
  - **Wall-clock fallback**: when no recent vsync is available, target
    is `g_fp_last_wait_return_us + 16667`. Same precision, just no
    panel-aligned phase.
- The wait is implemented as a single
  `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL)`
  syscall, restarted on `EINTR`. This gives kernel-scheduler-precision
  wake-ups with no thread-hop variance.

What this is **not** doing:
- It does not track `presentId` completion. DXVK's `presentId` is
  recorded in `g_present_records` (for diagnostic use only) but the
  wait doesn't block on it.
- It does not predict DXVK's render time and scale the wait. An earlier
  iteration did, which created a feedback loop during shader compile
  (predicted climbed → pacing slowed → render times measured against
  the slowed pacing → predicted stayed high → 40 s recovery). The
  current implementation always targets the next vsync.

Effect: `vkWaitForPresentKHR` returns on a panel-vsync-aligned boundary
roughly one period after the previous return, which DXVK uses to pace
its render submission. Slow renders auto-throttle via Acquire's
back-pressure on full pipelines, not via WaitForPresent.

**Earlier attempts that didn't work** (kept here so the design history
is recoverable):

**Attempt 1 — track `presentId` → slot via a ring buffer, advance
`max_completed_present_id` on each `MSG_RELEASE`, block `WaitForPresentKHR`
on it.** Caused "time-travel" (enemies moving backward briefly) because
`MSG_RELEASE` doesn't carry a `presentId` — the "mark newest matching
slot as completed" guess advanced the counter ahead of the actual
displayed frame. DXVK saw multiple waits return at once, submitted bursts
of frames, mailbox dropped non-monotonically, the game's dt-based physics
mis-extrapolated.

**Attempt 2 — extend the IPC protocol.** Added `present_id` to
`MSG_PRESENT` and a `displayed` flag to `MSG_RELEASE` (1 for
`onComplete`-driven releases, 0 for mailbox-drain drops). The
release-reader thread bumped `g_display_count` only on `displayed=1`
releases; `WaitForPresentKHR` blocked until it ticked.

Result: framerate dropped to a hard 30 FPS locked to a single slot.
Why:
- DXVK's strict FIFO loop is **1-buffer-deep**: acquire → render →
  present → wait → repeat.
- `onComplete` fires ~1 vsync after `apply` (the buffer is "displayed"
  only after being latched at the next vsync).
- Each Wine iteration therefore takes ≥ 2 vsyncs.
- With 1-buffer cycling, only the most-recently-released slot is free,
  so Wine reuses the same slot indefinitely.

The release-chain pacing was abandoned for the phase-anchored approach
above. The IPC extensions (`present_id` field on `MSG_PRESENT`,
`displayed` flag on `MSG_RELEASE`) are kept in the protocol because
they're useful for any future `ASurfaceTransaction_setOnCommit`-driven
work, but they're not consumed by the current pacing.

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
    int32_t  acquire_fd;    // SYNC_FD; render-complete (direct-render)
                            // or blit-complete (trojan-blit); -1 if none
    int32_t  dst_x, dst_y, dst_w, dst_h;
    uint64_t present_id;    // DXVK's VkPresentIdKHR; 0 if not provided
                            // (carried for future setOnCommit pacing)
    uint8_t  bgra_bytes;    // always 1 since pool unified to HAL_BGRA;
                            // tells receiver to display BGRA-ordered bytes
};

struct release_msg {
    uint8_t  type;          // MSG_RELEASE (2)
    uint32_t slot_index;
    int32_t  release_fd;    // currently -1
    uint8_t  displayed;     // 1 = onComplete (actual vsync tick)
                            // 0 = mailbox drop (slot freed, not displayed)
                            // (carried for future setOnCommit pacing)
    uint64_t vsync_time_ns; // 0 unless type == MSG_VSYNC
};

// MSG_VSYNC (3) — same struct as release_msg, sent by Android's
// AChoreographer callback. The release-reader thread treats it as a
// pure timekeeping signal: updates g_last_hardware_vsync_us = mono_us()
// (NOT vsync_time_ns — recv time gives implicit SF deadline margin),
// does not touch slot state. Drives the phase-anchored WaitForPresent.
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
layer_QueuePresentKHR                           — extract presentId from pNext.
                                                  Direct-render: submit zero-cmd
                                                  fence-only signal waiting on
                                                  DXVK's render-complete semaphore.
                                                  Trojan-blit: record per-slot
                                                  command buffer with barriers +
                                                  same-format vkCmdBlitImage
                                                  (trojan B8G8R8A8 → AHB
                                                  B8G8R8A8), submit waiting on
                                                  DXVK's semaphore.
                                                  Both: ship sync_fd via
                                                  wine_ahb_queue_present.
layer_WaitForPresentKHR                         — phase-anchored vsync alignment
                                                  via clock_nanosleep(ABSTIME).
                                                  Anchor updated by MSG_VSYNC
                                                  on release-reader thread.
                                                  See "Frame pacing — current
                                                  state" above.
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
reset per-slot fence (release chain guarantees previous use is done)

if g_direct_render_mode:
    # Direct-render: DXVK already wrote final image into AHB. Just turn
    # render-complete semaphores into a SYNC_FD.
    submit ZERO command buffers
        wait on pPresentInfo->pWaitSemaphores (BOTTOM_OF_PIPE)
        signal per-slot fence (SYNC_FD-exportable)
else if trojan-blit infrastructure exists for this slot:
    record per-slot command buffer:
      barrier  trojan: PRESENT_SRC_KHR  → TRANSFER_SRC_OPTIMAL
      barrier  AHB:    UNDEFINED        → TRANSFER_DST_OPTIMAL
      vkCmdBlitImage trojan → AHB  (both BGRA, NEAREST; effective byte copy)
      barrier  trojan: TRANSFER_SRC_OPTIMAL → PRESENT_SRC_KHR (restore for DXVK)
      barrier  AHB:    TRANSFER_DST_OPTIMAL → GENERAL          (for SF handoff)
    submit cmd buffer
        wait on pPresentInfo->pWaitSemaphores (TRANSFER stage)
        signal fence
else:
    # Fallback (no per-slot resources): drain semaphores, no copy.
    # Only reachable if FIFO cap path failed; should be unreachable now.
    submit empty drain submit with NULL fence

update g_presented_ring (history of last 2 slots, used by acquire bias)
wine_ahb_queue_present(swap, queue, slot, fence, presentId, bgra_bytes=1)
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

Tested on Odin 2 Portal, Android 13, Adreno 740, Proton 9.0, DXVK 2.x.
"Q" = Quality (direct-render), "P" = Performance (trojan-blit), "N" =
Native X11 (DAC layer disabled).

| Game | Present mode | Q | P | N | Notes |
|---|---|---|---|---|---|
| Broforce | IMMEDIATE | ✅ 60 fps | ✅ 60 fps | ✅ | Both DAC modes jitter-free after pacing rewrite |
| Vampire Survivors | FIFO | ✅ 60 fps | ✅ 60 fps | ✅ | P required the FIFO image-count cap to stop the freeze-at-start |
| Hollow Knight | FIFO | ✅ | ✅ | ✅ | |
| GTA4 | FIFO | ✅ | ✅ | ✅ | |
| Megabonk | FIFO | ⚠️ game-side hang | ⚠️ game-side hang | 💥 Unity crash | Game-side incompatibility (Unity + Steamworks SDK + Rewired plugin from SD-card); not a Winlator regression. Both DAC modes presented 2000+ steady frames before the game logic stopped advancing. Native run triggered `UnityCrashHandler64.exe` ~12 s in. |

---

## Known issues / future work

1. **Frame pacing jitter — substantially reduced, not eliminated.** The
   `clock_nanosleep(ABSTIME)` + MSG_VSYNC phase anchor rewrite produced
   the largest single jitter improvement so far; on Broforce and the
   FIFO test set, motion now matches what the X11 path delivered. Any
   remaining variance comes from DXVK render-time variance (box64 +
   shader compile) rather than from layer-side pacing. The
   `setOnCommit`-driven model (display-tick rather than vsync-tick)
   remains a worthwhile follow-up if a game with strict 1-frame-input
   tolerance shows up.

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

7. **`XServerDisplayActivity.exit()` busy-wait.** Lines ~634-638 spin
   on `ProcessHelper.listRunningWineProcesses().isEmpty()` with a
   1.5 s timeout but no `Thread.sleep` in the loop, blocking the UI
   thread. Reproducible as "Winlator freezes when I exit via the
   sidebar." Fix: add `Thread.sleep(50)` per iteration. Not a DAC
   issue (pre-existing) but tracked here because the freeze is what
   users see.

---

## Key lessons

**1. A swapchain-extension override must cover the whole contract.** When
a Vulkan layer replaces the implementation of `vkQueuePresentKHR`, it
must also handle every dependent function the extension introduces.
`VK_KHR_present_wait` defines `vkWaitForPresentKHR`; replacing present
without also handling the wait broke FIFO games for weeks. The same
pattern will recur for any future extension that creates an async "the
present reached the display" expectation. The diagnostic that finally
exposed it was DXVK's own `info`-level log listing the enabled extensions
— enable `DXVK_LOG_LEVEL=info` first for any hang of this shape.

**2. The interceptor's index space must match the host's index space.**
The trojan-blit FIFO freeze on Vampire Survivors came from
`layer_AcquireNextImageKHR` returning AHB-pool indices (0-3) while the
real swapchain had a smaller image count (2-3 for FIFO). Slots beyond
the trojan count had `VK_NULL_HANDLE` per-slot resources; the present
fell through to a fallback that shipped stale AHBs. A layer that
fabricates its own buffer pool must clamp its exposed index space to
the host's, not assume they match.

**3. Format-aware blit ≠ format-correct AHB.** Initial trojan-blit used
`vkCmdBlitImage` to swap R↔B implicitly between a BGRA source and an
RGBA AHB. That worked, but the moment a second pipeline (direct-render)
also wanted to write into the same AHB pool, the asymmetry forced
either a different pool per mode or per-frame swapping. Unifying the
AHB pool to `HAL_PIXEL_FORMAT_BGRA_8888` and importing as
`VK_FORMAT_B8G8R8A8_UNORM` made both pipelines see the same byte
layout, and the `present_msg.bgra_bytes` flag tells the receiver to
display it as such. Simpler infrastructure, fewer edge cases.

**4. The UI source of truth must be enforced after env-var merges.**
Adding the Graphics Pipeline dropdown was straightforward; making it
actually win against legacy shortcut envVars was not. The shortcut's
hand-written `envVars=WINLATOR_AHB_DIRECT_RENDER=1` line was being
merged in *after* our pipeline-derived value, silently pinning the
mode regardless of the spinner. Pattern: any setting controlled by a
UI element must be re-asserted in `GuestProgramLauncherComponent`
after the `envVars.putAll(this.envVars)` merge. A single
`Log.i("Graphics Pipeline resolved: ...")` line on launch made the
problem visible — without it the bug would have looked like "the modes
do the same thing."
