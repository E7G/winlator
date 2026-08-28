# Winlator Ludashi · E7G 整合版

这是基于 **Winlator Ludashi 3.1** 的可维护整合分支，目标是保留 3.1 的新渲染/输入/存储修复，同时吸收 Ludashi Plus 中仍然适用于 3.1 的优化思路，并提供中文界面、可复现构建和自动 Release。


## 3.1-E7G.5：高画质通用超分

- 新增 **Snapdragon GSR · 高画质（边缘方向）**：基于 Qualcomm 公开 SGSR1 edge-direction 变体，使用 12-tap Lanczos-like 重建、边缘方向权重和自适应锐化。
- 保留原 **Snapdragon GSR · 性能**，适合优先帧率/功耗。
- 滤镜现在有：双线性、最近邻、SGSR 性能、SGSR 高画质。
- 高画质超分仍是单帧空间超分，不依赖游戏提供 motion vector/depth，因此可以对 DXVK/Wine 游戏通用使用。
- 开启任何超分滤镜时会自动退出 AHB Direct Render，走 VulkanRenderer 完成重建；关闭超分后 Direct Render 可再次命中。
- 已将 Snapdragon GSR BSD-3-Clause 许可证随 APK 一起打包。

说明：Qualcomm 当前更高画质的 SGSR2 属于时域超分，需要低分辨率 color、depth 和 motion vectors。Winlator 最终合成层没有这些游戏内部缓冲，因此没有伪装成“通用 SGSR2”。

## 3.1-E7G.4：端到端 DXVK AHB + Proton 11

- **端到端 DXVK AHB Direct Render**：DXVK 的 gameplay swapchain 直接使用 AHardwareBuffer-backed VkImage，渲染完成后通过 SYNC_FD 交给 SurfaceFlinger；命中时不再经过 Winlator Vulkan 合成，也不需要中间 blit。
- **动态 AHB swapchain pool**：游戏运行中改变分辨率时可重建 2–4 个 AHB 并把新 handles 返回 DXVK Layer，不再因为分辨率不同直接失去 DAC。
- **三级自动回退**：端到端 DXVK AHB → DRI3 AHB 直出 → 3.1 VulkanRenderer，兼容性优先。
- **Vulkan Layer 每次 CI 现编**：移除旧预编译 `libahb_layer.so`，避免 Java/native 协议和 APK 内二进制版本漂移。
- **内置 Proton 更新到 11.0-2 ARM64EC SDK28**：GitHub Actions 固定 release tag + SHA-256，构建时将 WCP 正规化成离线内置 Wine 资产。
- 在线内容源同时提供 **Proton 11.0-2 SDK28/35** 与 **GE-Proton11-5 SDK28/35**。

启用端到端模式需要同时打开：

1. 全局设置里的 **端到端 DXVK AHB Direct Render（实验）**；
2. 当前容器/快捷方式渲染器选项里的 **Native Rendering+（AHB 直出）**。

详细原理、回退条件和调试方法见 [极致性能模式与 AHB Direct Render](docs/PERFORMANCE.zh-CN.md)。

## 3.1-E7G.3 极致性能增强

- **Native Rendering+（AHB 直出）**：符合条件的 DRI3 AHardwareBuffer 帧直接提交给 SurfaceFlinger，绕过 Winlator Vulkan 合成与 swapchain present；不兼容场景自动回退原 VulkanRenderer。
- **Root 极致性能模式**：可选地在游戏会话期间提高 CPU/GPU 频率策略和 Winlator/Wine 进程调度优先级，退出后恢复；不会主动关闭温控、SELinux 或充电保护。
- Native Rendering+ 支持独立光标层，Android 10 / API 29+ 可启用直出路径。

详细启用方法、回退条件和调试说明见 [极致性能模式与 Native Rendering+](docs/PERFORMANCE.zh-CN.md)。

## 分支

- `main`：稳定发布线（通过 CI 后再更新）。
- `integration/ludashi-3.1-plus`：当前 3.1 整合、汉化与构建验证分支。
- `legacy/ludashi-plus-dac`：完整保留 Ludashi Plus 的实验 Direct Android Compositing / AHardwareBuffer 实现，便于后续针对 3.1 新渲染架构重新移植。
- 原来的 E7G 上游聚合结构会在切换主线前保留备份分支。

## 已整合/保留

- Ludashi 3.1 最新修复：可移动存储映射、FEX 环境变量、相对鼠标/CPU affinity、新 VulkanRenderer 架构等。
- Ludashi Plus 已被 3.1 吸收的退出不卡 UI 优化。
- 3.1 中更新的 Box64/WOWBox64 0.4.2、Turnip 26.2.0 等组件，不回退到 Plus 的旧版本。
- 简体中文 UI 资源（`values-zh-rCN`），当前 `strings.xml` 588/588 键全覆盖。
- Gradle 8.10.2 构建链。
- GitHub Actions 自动编译 APK；`main` 构建成功后自动创建 GitHub Release 并上传 APK。
- 在线组件采用主源 + E7G 精选补充源：Box64/WOWBox64 0.4.4、FEX 2608、DXVK 3.0.2、VKD3D-Proton 3.0.1；Turnip 驱动源提供 26.3.0 Nightly。

## 关于 Ludashi Plus DAC

Plus 的 DAC/AHardwareBuffer 零拷贝方案基于 Ludashi 3.0 的旧 scanout 架构；Ludashi 3.1 在 2026-06-29 已移除该套 scanout 残留并重构 VulkanRenderer。直接覆盖会重新引入被删除的旧路径并与 3.1 C++/JNI 大面积冲突，因此稳定线不会用旧文件整块覆盖。

实验实现已完整保存在 `legacy/ludashi-plus-dac`，后续可以针对 3.1 的现有渲染架构逐步重接 AHB/DAC，而不是牺牲 3.1 的稳定性。

## 组件更新原则

Winlator 使用的是定制 Android/ARM64/ARM64EC 压缩包，官方桌面版 release 通常不能直接替换。仓库会区分“上游最新版”和“已验证可打包版本”，避免出现界面显示新版本但实际资源不存在、或升级后无法启动的问题。

详见 [组件状态](docs/COMPONENTS.zh-CN.md)。

## 构建

本地：

```bash
chmod +x gradlew
./gradlew --no-daemon assembleDebug
```

GitHub Actions 会安装所需 shader 工具、固定版本的 libadrenotools/linkernsbypass，然后生成 APK。

## 致谢

本项目基于 brunodev85/Winlator 及其衍生项目，并参考/整合 StevenMXZ/Winlator-Ludashi 与 TripleJ160/Winlator-Ludashi-Plus 的工作。请同时遵循各上游项目的许可证和第三方组件许可证。
