

<p align="center">
  <img src="logo.png" alt="Winlator Bionic" width="600">
</p>

# Winlator Bionic

Winlator is an Android application that lets you run Windows (x86\_64) applications with Wine. It supports standard `x86_64` containers using Box86/Box64, as well as `Arm64EC` containers which utilize FEXCore (for 64/32-bit) or an optional WowBox64 (for 32-bit).

This is a fork of the **Winlator Bionic** project by [Pipetto-crypto](https://github.com/Pipetto-crypto/winlator), continued from [StevenMXZ's Winlator-Ludashi 3.0](https://github.com/StevenMXZ/Winlator-Ludashi).

and backup [Ludashi-backup](https://github.com/StevenMX-backup/Ludashi-Backup).

## ✨ Why try this fork?

The headline feature is **Direct Android Compositing (DAC)** — a new Vulkan present path that routes frames directly from DXVK to SurfaceFlinger, skipping the X11 server entirely for Vulkan-rendered games. What you actually feel:

- 🎮 **Lower input latency.** One fewer compositor stage between the GPU and the display. Controls feel more responsive in fast-paced games (Broforce, Hollow Knight, GTA IV tested).
- 🌊 **Smoother motion.** Vsync-aligned absolute-time sleeps replaced the previous render-pacing model. The historical "Performance mode jitter" on Vulkan games is largely gone on tested hardware.
- 🔋 **Less GPU work in the compositor.** On overlay-capable devices (most Adreno phones), SurfaceFlinger picks the hardware overlay path — your game's frame goes to the display panel without any extra GPU compositing pass.
- 🩹 **FIFO Vulkan games that froze in Performance mode now work.** Vampire Survivors, Hollow Knight, GTA IV all run cleanly (fixes a slot-index mismatch in the trojan-blit pipeline).
- 🔓 **Lock/unlock no longer leaves a black screen.** A long-standing lifecycle bug where the compositor stayed detached after device sleep is fixed.
- 🎚️ **Per-game pipeline picker.** A new dropdown in Container Settings and per-shortcut Settings lets you choose **Quality (Direct-Render)**, **Performance (Trojan-Blit)**, or **Native (X11)** per game. If a title misbehaves under DAC, switch to Native in five seconds — no uninstall, no container changes.
- ⏪ **Guaranteed upstream fallback.** The Native (X11) option is byte-for-byte the same code path as upstream Ludashi 3.0. You can never regress below upstream — if anything works there, you can always reach it here through the dropdown.

**Safe to try.** If you're already running upstream Ludashi 3.0, your save data and Wine prefix are unaffected — the new dropdown defaults to Quality on existing containers but you can flip any individual shortcut back to Native at any time. Worst case, you set everything to Native and you're back to upstream behavior.

## ⚠️ Experimental — please read

This fork (`winlator-ludashi-plus`) is my **first contribution to the Winlator project** and **my first Android development project, period**. The Direct Android Compositing pipeline added here is non-trivial low-level work (Vulkan layer, AHardwareBuffer, SurfaceControl, IPC), and while every change has been verified end-to-end on my hardware, much of it is genuinely experimental.

What this means in practice:

- **Game compatibility may vary.** Vulkan games that work on upstream Ludashi 3.0 should still work here, but the DAC pipeline introduces a new code path. If a game misbehaves under the default *Quality (Direct-Render)* mode, try *Performance (Trojan-Blit)*, and if that still fails, switch to *Native (X11)* to fall back to upstream behavior. The dropdown is designed exactly for this.
- **Bugs are likely.** Especially on hardware different from mine (Odin 2 Portal — Adreno 740, Android 13). Please open issues with logcat snippets and the Graphics Pipeline setting you were using.
- **Performance is hardware-dependent.** Quality mode squeezes out the lowest latency on overlay-capable devices; Performance mode can be steadier on weaker GPUs. Try both per-game via the per-shortcut setting.
- **Not a stable release.** Treat as a beta. Save game progress before testing on titles you care about.

If you hit something that worked on upstream Ludashi 3.0 but doesn't here, that's a regression and I want to know — open an issue. If you hit something that didn't work upstream either, that's likely a Wine/Proton/game compatibility limit and the *Native (X11)* dropdown option will give you the same behavior as upstream.
## APK Build Explanations

### what is Ludashi?

The Ludashi Build is functionally identical to the standard Bionic app, but the package name has been renamed to mimic Ludashi, a popular benchmark app. Some Android phones — especially Xiaomi devices — may automatically enable performance mode when such apps are detected, potentially reducing throttling and boosting performance slightly.

### Dev-Vanilla Build

This is the standard, unmodified build. It uses the original package name, which allows it to be installed alongside other popular Winlator forks (like the coffincolors version) without any package conflicts.

### RedMagic Build

This build mimics the package name of Genshin Impact. This is specifically designed for RedMagic devices, as the phone's software may detect this package name to enable hardware-specific gaming enhancements, such as built-in frame generation (framegen). Using this build may unlock these features and improve performance on supported RedMagic phones.

# Installation

1.  Download and install the latest APK from this repository's [Releases section](https://github.com/StevenMXZ/Winlator-Ludashi/releases) (choose your preferred build: `dev-vanilla`, `ludashi`, or `redmagic`).
2.  Launch the app and wait for the installation process to finish.

# Graphics Pipeline (Direct Android Compositing)

This fork adds **Direct Android Compositing (DAC)** — a zero-copy Vulkan
present path that routes `DXVK → AHardwareBuffer → SurfaceControl → SurfaceFlinger`
instead of going through Wine's X11 server. The result is lower latency
and smoother motion for Vulkan-rendered games.

A **Graphics Pipeline** dropdown is available in both Container Settings
and per-shortcut Settings (Shortcuts → ⋮ → Settings → Graphics Pipeline)
with three options:

| Option | What it does | When to pick it |
|---|---|---|
| **Quality (Direct-Render)** *(default)* | DXVK renders directly into AHardwareBuffers, no intermediate copy. SurfaceFlinger composites via hardware overlay. | Default for all Vulkan games. Lowest latency, smoothest motion. |
| **Performance (Trojan-Blit)** | DXVK renders into a regular device-local image; layer blits it to the AHB before display. | Compatibility fallback if Quality has artifacts on a specific game; sometimes yields slightly higher steady FPS on shader-heavy titles. |
| **Native (X11)** | DAC layer is disabled entirely (`DISABLE_AHB_LAYER=1`). Game runs through the original Wine-X11 path. | Use for games that misbehave under DAC, or to compare against upstream Winlator behavior. |

The shortcut-level setting wins over the container-level setting when set,
so you can keep most games on Quality and switch individual problem
titles to Performance or Native without changing the container default.

### Where to find it

**Container-wide default** — Container Settings → Edit Container → *Graphics Pipeline*:

<p align="center">
  <img src="docs/screenshots/graphics-pipeline-container.png" alt="Graphics Pipeline dropdown in Container Settings" width="800">
</p>

**Per-game override** — Shortcuts → ⋮ on the game → Settings → *Graphics Pipeline*:

<p align="center">
  <img src="docs/screenshots/graphics-pipeline-shortcut.png" alt="Graphics Pipeline dropdown in Shortcut Settings" width="800">
</p>

# Useful Tips

  - Here is a tutorial from ZeroKimchi channel on how to use Winlator Bionic:
    https://youtu.be/EJDWZUGF9sk?si=e3Z-DdmMJSYKduWz
  - If you are using an `x86_64` container and experiencing performance issues, try changing the Box86/Box64 preset to **Performance** in Container Settings -\> Advanced Tab.
  - If you are using an `Arm64EC` container, try swapping between different FEXCore versions (2505,2507 etc) in the container settings for better compatibility or performance.
  - For applications that use .NET Framework, try installing Wine Mono found in Start Menu -\> System Tools.
  - If some older games don't open, try adding the environment variable MESA\_EXTENSION\_MAX\_YEAR=2003 in Container Settings -\> Environment Variables.
  - Try running the games using the shortcut on the Winlator home screen, there you can define individual settings for each game.
  - To speed up the installers, try changing the Box86/Box64 preset to Intermediate in Container Settings -\> Advanced Tab.
  - If a Vulkan game freezes or crashes, try switching the **Graphics Pipeline** option (Quality → Performance → Native) in the shortcut settings before assuming Wine/Proton compatibility is the problem.

# Additional Components & Updates

You can find updated components (known as `wcps`) to improve compatibility and performance, as well as new drivers, at the links below:

  - **Winlator Components (FEXCore, Box64/Box86, DXVK, etc.):**
      - [StevenMXZ's Winlator-Contents Repository](https://github.com/StevenMXZ/Winlator-Contents)
  - **Adreno GPU Drivers (Turnip):**
      - [Kimchi's AdrenoToolsDrivers Releases](https://www.google.com/search?q=https://github.com/K11MCH1/AdrenoToolsDrivers/releases)

# Credits and Third-party apps

  - **Original Winlator** by [brunodev85](https://github.com/brunodev85/winlator)
  - **Original Winlator Bionic** by [Pipetto-crypto](https://github.com/Pipetto-crypto/winlator)
  - **Winlator (coffincolors fork)** by [coffincolors](https://github.com/coffincolors/winlator)
  - Ubuntu RootFs (Bionic Beaver): [releases.ubuntu.com/bionic](https://www.google.com/search?q=https://releases.ubuntu.com/bionic)
  - Wine: [winehq.org](https://www.winehq.org/)
  - Box86/Box64 by [ptitseb](https://github.com/ptitSeb)
  - FEX-Emu by [FEX-Emu](https://github.com/FEX-Emu/FEX)
  - PRoot: [proot-me.github.io](https://proot-me.github.io)
  - Mesa (Turnip/Zink/VirGL): [mesa3d.org](https://www.mesa3d.org)
  - DXVK: [github.com/doitsujin/dxvk](https://github.com/doitsujin/dxvk)
  - VKD3D: [gitlab.winehq.org/wine/vkd3d](https://gitlab.winehq.org/wine/vkd3d)
  - D8VK: [github.com/AlpyneDreams/d8vk](https://github.com/AlpyneDreams/d8vk)
  - CNC DDraw: [github.com/FunkyFr3sh/cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw)
[ptitseb](https://github.com/ptitSeb) (Box86/Box64), [Danylo](https://blogs.igalia.com/dpiliaiev/tags/mesa/) (Turnip), [alexvorxx](https://github.com/alexvorxx) (Mods/Tips) and others.








