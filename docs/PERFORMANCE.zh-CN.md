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
