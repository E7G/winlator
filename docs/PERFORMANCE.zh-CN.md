# 极致性能模式与 Native Rendering+（3.1-E7G.3）

本版本在 Ludashi 3.1 新 `VulkanRenderer` 架构上重新实现 AHardwareBuffer 直出，不恢复 3.0/Plus 的旧 scanout 状态机。

## Native Rendering+ / AHB Direct

开启路径：

1. 编辑容器或快捷方式。
2. 打开“渲染器选项”。
3. 勾选 **Native Rendering+（AHB 直出）**。
4. 正常启动游戏。

符合条件的 DRI3 Present 帧会走：

```
Wine / DXVK
  -> DRI3 GPUImage
  -> AHardwareBuffer
  -> DirectAHBCompositor
  -> ASurfaceTransaction_setBuffer
  -> SurfaceFlinger
  -> Display
```

热路径不再经过：

```
AHB -> Vulkan external-memory texture import
    -> shader sampling / compositor pass
    -> Winlator Vulkan swapchain
    -> vkQueuePresentKHR
```

因此在直出命中的帧上，可以减少一次 Winlator Vulkan 合成和 swapchain present 开销。

### 自动回退

Native Rendering+ 是 opt-in，但不是“强制直出”。以下场景会自动隐藏 DAC 层并回退到 3.1 原生 VulkanRenderer：

- 当前帧不是 DRI3 AHardwareBuffer。
- 窗口/帧不能覆盖容器画面。
- 启用了特殊纹理滤镜。
- 启用了 stretch / PostFX。
- 启用了红蓝通道交换。
- 放大镜不为 1.0。
- 启用了随光标移动的屏幕偏移。
- Android 进入画中画。
- SurfaceControl / AHardwareBuffer 提交失败。

回退发生在同一套 renderer 中，不需要重启容器。DAC 失败时原有 `nativeUpdateWindowContentAHB()` 路径仍然保留。

### 系统要求

- AHardwareBuffer：Android 8 / API 26 起。
- Native Rendering+ SurfaceControl 直出：Android 10 / API 29 起。
- API 低于 29 时自动使用 VulkanRenderer。

### 光标

3.1-E7G.3 的 `DirectAHBCompositor` 使用单独的 SurfaceControl cursor layer。游戏帧直出时，X11/Wine 光标仍可更新位置、热点和可见性，不需要为了 DAC 隐藏鼠标。

## 3.1-E7G.4：端到端 DXVK AHB Direct Render

3.1-E7G.4 在上一版 DRI3 AHB 直出的基础上继续向前移动生产端：DXVK gameplay swapchain 本身可以由 AHardwareBuffer-backed VkImage 组成。

命中后的热路径：

```
D3D9/10/11
  -> DXVK draw commands
  -> AHardwareBuffer-backed VkImage
  -> render-complete semaphore
  -> exportable Vulkan fence / SYNC_FD
  -> DirectAHBCompositor
  -> ASurfaceTransaction_setBuffer
  -> SurfaceFlinger
  -> Display
```

这条路径不需要：

- DXVK 普通 Android/X11 WSI swapchain present。
- AHB -> Winlator Vulkan texture import。
- Winlator compositor shader sampling。
- Winlator Vulkan swapchain。
- 中间 `vkCmdBlitImage`（端到端 direct mode 命中时）。

### 启用条件

需要同时打开两个开关：

1. **设置 -> 端到端 DXVK AHB Direct Render（实验）**。
2. **容器/快捷方式 -> 渲染器选项 -> Native Rendering+（AHB 直出）**。

这样设计是为了让端到端模式保持逐容器 opt-in；只打开全局实验开关不会影响没有启用 Native Rendering+ 的容器。

### 动态分辨率

DXVK 创建的 gameplay swapchain 如果与初始 AHB pool 尺寸不一致，Layer 会发送 `MSG_REALLOC`。Android 端会：

1. 校验分辨率和 image count。
2. 分配新的 BGRA AHardwareBuffer pool。
3. 发送 `MSG_REALLOC_ACK`。
4. 连续发送新的 AHB handles。
5. DXVK Layer 重新导入新 pool。

目前限制单边最大 8192 像素，pool 数量 2–4。

### Vulkan AHB import

3.1-E7G.4 不再猜测 AHB 的 Vulkan memory type，也不再使用 `allocationSize=0`。现在会通过：

`vkGetAndroidHardwareBufferPropertiesANDROID`

取得真实的：

- `allocationSize`
- `memoryTypeBits`

再选择兼容的 device-local memory type。这对较严格的 Turnip/Vulkan 驱动尤其重要。

### 色彩格式

Android pool 使用 `HAL_PIXEL_FORMAT_BGRA_8888`，Vulkan 侧以 `VK_FORMAT_B8G8R8A8_UNORM` 导入。因此 DXVK 写入的 BGRA byte layout 可以直接被 SurfaceFlinger 消费，不需要额外 R/B shader 或 CPU 转换。

### 同步

DXVK 的 render-complete semaphore 会被一个零 command-buffer submit 消耗，并产生 exportable fence；Layer 将 fence 导出为 SYNC_FD，SurfaceFlinger 在扫描 AHB 前等待该 fence。旧 slot 在 SurfaceControl transaction complete 后通过 `MSG_RELEASE` 回到 DXVK pool。

### 自动回退

端到端模式失败不会要求重启游戏。设计上的回退顺序为：

```
DXVK AHB direct
  -> 3.1 DRI3/GPUImage AHB direct
  -> VulkanRenderer AHB import/compositor
```

设备不支持 `VK_ANDROID_external_memory_android_hardware_buffer`、SurfaceControl 创建失败、socket 握手失败或 gameplay swapchain 无法接管时，Layer 会保留/恢复普通 Vulkan WSI 路径。

## Proton 11

3.1-E7G.4 的新内置默认 Wine 为：

`Proton 11.0-2 ARM64EC SDK28`

构建时固定使用 The412Banner 的 `build-p11-20260821` release，并校验 WCP SHA-256：

`23ae9d8ff61b4a3fc619bffc3e6ba810e89cb33bb319af29f8e4320e59f3259d`

WCP 的 `profile.json.wine.binPath` 会在 CI 中解析，运行时根目录被重新打包为 Winlator 原生的 `/opt/proton-11.0-2-arm64ec` 布局，并包含对应 `prefixPack.txz`。

已有安装升级时不会删除 Proton 9/10 或重置整个 imagefs；如果检测到 Proton 11 缺失，只补装新的内置 Proton。

在线内容源同时加入：

- Proton 11.0-2 ARM64EC SDK28
- Proton 11.0-2 ARM64EC SDK35
- GE-Proton11-5 ARM64EC SDK28
- GE-Proton11-5 ARM64EC SDK35


## Root 极致性能模式

位置：

**设置 -> 实验性功能 -> Root 极致性能模式**

需要设备提供可用的 `su`（例如常见 Root 管理方案）。未开启此选项时 Winlator 不会主动请求 Root 权限。

开启后，在游戏会话期间会进行 best-effort 调优：

- CPU cpufreq policy：有 `performance` governor 时切换到 performance。
- CPU：把 `scaling_min_freq` 临时提高到当前 `scaling_max_freq`。
- GPU devfreq：识别 KGSL / Adreno / GPU devfreq 节点。
- GPU：有 performance governor 时启用，并临时提高最低频率。
- Qualcomm KGSL：可用时开启会话期间的 clock / bus / rail keep-on。
- Winlator / Box64 / FEX / Wine 子进程：提高 nice 优先级，并请求高 I/O 优先级。
- 游戏启动后再次扫描子进程树，覆盖稍后创建的 Wine/游戏进程。

### 可恢复设计

首次修改每个 sysfs 节点前会记录原值。退出游戏、Activity 销毁或下次启用 Root 模式时，会先执行恢复脚本，再重新应用性能参数。

状态文件位于：

```
/data/local/tmp/winlator-e7g-rootperf-<uid>.state
```

正常退出后会删除。

### 不会做的事

Root 极致性能模式**不会**主动：

- 关闭 thermal / 温控服务。
- 禁用 SELinux。
- 关闭电池或充电保护。
- 修改内核电压。
- 永久写入系统性能节点。
- 在 Root 模式关闭时弹出 `su` 授权。

这样保留系统安全边界，同时尽可能减少调度和频率爬升带来的帧时间波动。

## 推荐组合

追求高帧率/低延迟时可优先尝试：

- Native Rendering+：开启。
- Root 极致性能模式：有 Root 且散热条件允许时开启。
- 高刷新率模式：开启。
- Present Mode：先试 Mailbox；出现兼容问题改 FIFO。
- Turnip：选择适合 SoC 的新版驱动。
- DXVK：优先使用当前在线源中的新版构建，并按游戏分别测试。
- PostFX/特殊滤镜：追求 DAC 命中率时关闭。

Root 高频运行会显著增加功耗和发热，即使系统温控保持开启，也建议保证足够散热。

## 调试

Logcat 关键 tag：

- `Winlator_DirectAHB`：SurfaceControl DAC 初始化、提交和回退。
- `Winlator_Renderer`：VulkanRenderer。
- `GPUImage`：AHardwareBuffer。
- `RootPerformance`：Root 调优和恢复。

如果 Native Rendering+ 没有命中，先确认游戏是否实际使用 DRI3/AHardwareBuffer；不满足条件时回退 Vulkan 是预期行为。
