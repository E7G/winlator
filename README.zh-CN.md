# Winlator Ludashi · E7G 整合版

这是基于 **Winlator Ludashi 3.1** 的可维护整合分支，目标是保留 3.1 的新渲染/输入/存储修复，同时吸收 Ludashi Plus 中仍然适用于 3.1 的优化思路，并提供中文界面、可复现构建和自动 Release。

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
