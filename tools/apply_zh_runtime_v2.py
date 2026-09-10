#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
changes = []


def load(rel):
    p = ROOT / rel
    return p, p.read_text(encoding="utf-8")


def save(p, old, new):
    if new != old:
        p.write_text(new, encoding="utf-8")
        changes.append(str(p.relative_to(ROOT)))


def replace_once(text, old, new, label):
    if new in text:
        return text
    if old not in text:
        raise RuntimeError(f"missing patch anchor: {label}")
    return text.replace(old, new, 1)


def replace_all(text, mapping):
    for old, new in mapping.items():
        text = text.replace(old, new)
    return text

# ---------------------------------------------------------------------------
# 1) Mainland-friendly GitHub download chain.
# ---------------------------------------------------------------------------
p, text = load("app/src/main/java/com/winlator/cmod/contents/Downloader.java")
text = replace_once(
    text,
    '    private static final String PRIMARY_GITHUB_PROXY = "https://gh-proxy.com/";\n'
    '    private static final String SECONDARY_GITHUB_PROXY = "https://ghproxy.net/";',
    '    private static final String PRIMARY_GITHUB_PROXY = "https://wget.la/";\n'
    '    private static final String SECONDARY_GITHUB_PROXY = "https://gh-proxy.com/";\n'
    '    private static final String TERTIARY_GITHUB_PROXY = "https://ghproxy.net/";',
    "Downloader proxy constants"
)
text = text.replace(
    'if (host.equals("gh-proxy.com") || host.equals("ghproxy.net")) {',
    'if (host.equals("wget.la") || host.equals("gh-proxy.com") || host.equals("ghproxy.net")) {'
)
text = replace_once(
    text,
    '                candidates.add(PRIMARY_GITHUB_PROXY + address);\n'
    '                candidates.add(SECONDARY_GITHUB_PROXY + address);\n'
    '            } else if (host.equals("api.github.com")) {\n'
    '                // gh-proxy.com supports GitHub API requests; ghproxy.net is kept for file traffic only.\n'
    '                candidates.add(PRIMARY_GITHUB_PROXY + address);',
    '                candidates.add(PRIMARY_GITHUB_PROXY + address);\n'
    '                candidates.add(SECONDARY_GITHUB_PROXY + address);\n'
    '                candidates.add(TERTIARY_GITHUB_PROXY + address);\n'
    '            } else if (host.equals("api.github.com")) {\n'
    '                // Never send API credentials to file mirrors. This path is only used for public, unauthenticated metadata.\n'
    '                candidates.add(SECONDARY_GITHUB_PROXY + address);',
    "Downloader candidates"
)
save(p, load("app/src/main/java/com/winlator/cmod/contents/Downloader.java")[1], text)

# ---------------------------------------------------------------------------
# 2) Route runtime catalogs and component archives through Downloader too.
# ---------------------------------------------------------------------------
p, original = load("app/src/main/java/com/winlator/cmod/ui/settings/RuntimeSettingsSupport.kt")
text = original
text = replace_once(
    text,
    'import com.winlator.cmod.contents.ContentsManager\n',
    'import com.winlator.cmod.contents.ContentsManager\nimport com.winlator.cmod.contents.Downloader\n',
    "Downloader import"
)
text = text.replace('import okhttp3.OkHttpClient\n', '').replace('import okhttp3.Request\n', '')
text = replace_once(
    text,
    '            OkHttpClient().newCall(Request.Builder().url(url).build()).execute().use { response ->\n'
    '                if (response.isSuccessful) response.body?.string()?.let(manager::setRemoteProfiles)\n'
    '            }',
    '            Downloader.downloadString(url)?.let(manager::setRemoteProfiles)',
    "remote catalog downloader"
)
old_download = '''    val archive = File(context.cacheDir, "winz-${System.nanoTime()}")
    val downloaded = withContext(Dispatchers.IO) {
        try {
            OkHttpClient().newCall(Request.Builder().url(profile.remoteUrl).build()).execute().use { response ->
                if (!response.isSuccessful || response.body == null) {
                    false
                } else {
                    response.body!!.byteStream().use { input ->
                        FileOutputStream(archive).use { output -> input.copyTo(output, 64 * 1024) }
                    }
                    true
                }
            }
        } catch (_: Exception) {
            false
        }
    }
    if (!downloaded) return null
'''
new_download = '''    val archive = File(context.cacheDir, "winz-${System.nanoTime()}")
    val remoteUrl = profile.remoteUrl ?: return null
    val downloaded = withContext(Dispatchers.IO) {
        Downloader.downloadFile(remoteUrl, archive)
    }
    if (!downloaded) return null
'''
text = replace_once(text, old_download, new_download, "runtime archive downloader")
text = text.replace('import java.io.FileOutputStream\n', '')

old_display = '''private fun settingDisplayLabel(value: String): String =
    if (value == "Lanczos 2 (16-tap)") "Lanczos 2" else value

private fun settingFieldLabel(label: String): String = when (label) {
    "Graphics Driver" -> "OpenGL Driver"
    "Driver Version" -> "Vulkan Driver"
    else -> label
}
'''
new_display = '''private fun settingDisplayLabel(value: String): String = when (value) {
    "Lanczos 2 (16-tap)" -> "Lanczos 2"
    "Default" -> "默认"
    "System" -> "系统"
    "Custom" -> "自定义"
    "Automatic" -> "自动"
    "Low Latency" -> "低延迟"
    "Stable" -> "稳定"
    "Off" -> "关闭"
    "Classic" -> "经典"
    "Modern" -> "现代"
    "Disabled" -> "禁用"
    "Light" -> "浅色"
    "Dark" -> "深色"
    "Image" -> "图片"
    "Solid Color" -> "纯色"
    "disable" -> "禁用"
    "enable" -> "启用"
    "force" -> "强制"
    "Bilinear" -> "双线性"
    "Nearest neighbor" -> "最近邻"
    "Snapdragon Super Resolution" -> "Snapdragon 超分辨率"
    "AMD FidelityFX Super Resolution" -> "AMD FidelityFX 超分辨率"
    "none" -> "无"
    "partial" -> "部分"
    "full" -> "完整"
    "auto" -> "自动"
    "software" -> "软件"
    "compute" -> "计算着色器"
    "Builtin (Wine)" -> "内置（Wine）"
    "Native (Windows)" -> "原生（Windows）"
    "Normal (Load all services)" -> "正常（加载全部服务）"
    "Essential (Load only essential services)" -> "精简（仅加载必要服务）"
    "Aggressive (Stop services on startup)" -> "激进（启动时停止服务）"
    "Device" -> "设备默认"
    else -> value
}

private fun settingFieldLabel(label: String): String = when (label) {
    "Graphics Driver" -> "图形驱动"
    "Driver Version" -> "Vulkan 驱动"
    "Audio Driver" -> "音频驱动"
    "Oboe latency" -> "Oboe 延迟模式"
    "Oboe backend" -> "Oboe 后端"
    "Winlator HUD" -> "Winlator 性能叠加层"
    "Locale (LC_ALL)" -> "区域设置 (LC_ALL)"
    "MIDI SoundFont" -> "MIDI 音色库"
    "Fullscreen Stretched" -> "拉伸全屏"
    "Desktop Theme" -> "桌面主题"
    "Desktop Background" -> "桌面背景"
    "Mouse Warp Override" -> "鼠标指针捕获"
    "Screen Size" -> "屏幕分辨率"
    "Renderer" -> "渲染器"
    "Surface format" -> "表面格式"
    "Bypass X11" -> "绕过 X11"
    "Performance mode" -> "性能模式"
    "Present at refresh rate" -> "按刷新率呈现"
    "Present Mode" -> "呈现模式"
    "Renderer Driver" -> "渲染器驱动"
    "Texture Filter" -> "纹理过滤"
    "Vulkan Version" -> "Vulkan 版本"
    "GPU Name" -> "GPU 名称"
    "Max Device Memory" -> "最大显存"
    "Driver Present Mode" -> "驱动呈现模式"
    "Sync Frame" -> "同步帧"
    "Disable Present Wait" -> "禁用呈现等待"
    "Resource Type" -> "资源类型"
    "BCN Emulation" -> "BCN 模拟"
    "BCN Emulation Type" -> "BCN 模拟类型"
    "BCN Emulation Cache" -> "BCN 模拟缓存"
    "DX Wrapper" -> "DirectX 转译层"
    "DXVK Version" -> "DXVK 版本"
    "VKD3D Version" -> "VKD3D 版本"
    "VKD3D Feature Level" -> "VKD3D 功能级别"
    "Max Frame Latency" -> "最大帧延迟"
    "Async" -> "异步"
    "Async Cache" -> "异步缓存"
    "DDraw Wrapper" -> "DirectDraw 转译层"
    "32-bit Emulator" -> "32 位模拟器"
    "FEXCore Version" -> "FEXCore 版本"
    "FEXCore Preset" -> "FEXCore 预设"
    "Box64 Version" -> "Box64 版本"
    "WOWBox64 Version" -> "WOWBox64 版本"
    "Box64 Preset" -> "Box64 预设"
    "Exclusive Input" -> "独占输入"
    "Enable XInput" -> "启用 XInput"
    "Enable DInput" -> "启用 DInput"
    "Sync CPU Topology" -> "同步 CPU 拓扑"
    "Processor Affinity" -> "处理器亲和性"
    "Processor Affinity (32-bit apps)" -> "处理器亲和性（32 位应用）"
    "Startup Selection" -> "启动模式"
    else -> label
}
'''
text = replace_once(text, old_display, new_display, "settings display localization")

text = text.replace('Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)',
                    'Text(settingFieldLabel(label), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)')
text = text.replace('Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)',
                    'Text(settingFieldLabel(label), modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)')
text = text.replace('label = { Text(label) },', 'label = { Text(settingFieldLabel(label)) },')
text = text.replace('Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)',
                    'Text(settingFieldLabel(title), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)')
text = replace_all(text, {
    'selectedOption?.label ?: selectedId.ifBlank { "Choose a version" }': 'selectedOption?.label ?: selectedId.ifBlank { "选择版本" }',
    '"${option.type} • Downloading…"': '"${option.type} • 正在下载…"',
    '"${option.type} • Download"': '"${option.type} • 下载"',
})
save(p, original, text)

# ---------------------------------------------------------------------------
# 3) Container editor: localize direct Compose chrome while keeping stored IDs.
# ---------------------------------------------------------------------------
p, original = load("app/src/main/java/com/winlator/cmod/ui/container/ContainerEditorV2.kt")
text = original
text = text.replace('ContainerNavItemV2(item, category == item)', 'ContainerNavItemV2(containerCategoryLabelV2(item), category == item)')
text = replace_all(text, {
    '"Container settings"': '"容器设置"',
    '"Wallpaper image"': '"壁纸图片"',
    '"Custom image"': '"自定义图片"',
    '"Default wallpaper"': '"默认壁纸"',
})
anchor = 'private fun loadContainerGpuNamesV2(context: Context): List<String> {'
helper = '''private fun containerCategoryLabelV2(value: String): String = when (value) {
    "General" -> "常规"
    "Video" -> "显示"
    "Compatibility" -> "兼容性"
    "Input" -> "输入"
    "Advanced" -> "高级"
    else -> value
}

'''
if helper not in text:
    if anchor not in text: raise RuntimeError("missing container helper anchor")
    text = text.replace(anchor, helper + anchor, 1)
save(p, original, text)

# ---------------------------------------------------------------------------
# 4) Input Controls page.
# ---------------------------------------------------------------------------
p, original = load("app/src/main/java/com/winlator/cmod/ui/inputcontrols/InputControlsComposeHost.kt")
text = replace_all(original, {
    '"-- Select Profile --"': '"-- 选择配置 --"',
    '"Input Controls"': '"输入控制"',
    '"EXTERNAL CONTROLLERS"': '"外接控制器"',
    '"Profile"': '"配置"',
    '"Add profile"': '"添加配置"',
    '"Edit profile"': '"编辑配置"',
    '"Duplicate profile"': '"复制配置"',
    '"Remove profile"': '"删除配置"',
    '"Overlay Opacity"': '"按键叠加层透明度"',
    '"${controller.bindings} bindings"': '"${controller.bindings} 个绑定"',
    '"Remove controller"': '"移除控制器"',
})
save(p, original, text)

# ---------------------------------------------------------------------------
# 5) Game library + details.
# ---------------------------------------------------------------------------
p, original = load("app/src/main/java/com/winlator/cmod/ui/library/BitmapComposeCompat.kt")
text = original.replace('LibraryFilterChip(option.name, option == filter)', 'LibraryFilterChip(libraryFilterLabel(option), option == filter)')
text = replace_all(text, {
    '"No games match your search"': '"没有匹配搜索条件的游戏"',
    '"No games in this section"': '"此分类中没有游戏"',
    '"Library"': '"游戏库"',
    '"Lock screen orientation"': '"锁定屏幕方向"',
    '"Vertical mode"': '"竖屏模式"',
    '"Horizontal mode"': '"横屏模式"',
    '"More options"': '"更多选项"',
    '"Play"': '"启动游戏"',
    '"Unfavorite"': '"取消收藏"',
    '"Favorite"': '"收藏"',
    '"Configure"': '"配置"',
    '"Artwork"': '"封面图片"',
    '"Home screen"': '"主屏幕"',
    '"Clone"': '"克隆"',
    '"Export"': '"导出"',
    '"Remove from library"': '"从游戏库移除"',
})
filter_anchor = '@Composable\nprivate fun LibraryFilterChip(label: String, selected: Boolean, click: () -> Unit) {'
filter_helper = '''private fun libraryFilterLabel(value: LibraryFilter): String = when (value) {
    LibraryFilter.All -> "全部"
    LibraryFilter.Favorites -> "收藏"
    LibraryFilter.Recent -> "最近"
}

'''
if filter_helper not in text:
    if filter_anchor not in text: raise RuntimeError("missing library filter anchor")
    text = text.replace(filter_anchor, filter_helper + filter_anchor, 1)
save(p, original, text)

p, original = load("app/src/main/java/com/winlator/cmod/ui/library/LibraryEmptyStateCompat.kt")
text = replace_all(original, {
    'EmptyFilterChip("All", true)': 'EmptyFilterChip("全部", true)',
    'EmptyFilterChip("Favorites", false)': 'EmptyFilterChip("收藏", false)',
    'EmptyFilterChip("Recent", false)': 'EmptyFilterChip("最近", false)',
    '"Add games"': '"添加游戏"',
})
save(p, original, text)

p, original = load("app/src/main/java/com/winlator/cmod/ui/library/GameDetailComposeHost.kt")
text = replace_all(original, {
    '"Back"': '"返回"',
    '"Favorite"': '"收藏"',
    '"Configure"': '"配置"',
    '"Enter container"': '"进入容器"',
    '"Game folder"': '"游戏目录"',
    '"Remove"': '"移除"',
})
save(p, original, text)

p, original = load("app/src/main/java/com/winlator/cmod/ui/library/LandscapePagerCore.kt")
text = original.replace('Icon(Icons.Outlined.PlayArrow, "Play")', 'Icon(Icons.Outlined.PlayArrow, "启动游戏")')
save(p, original, text)

# ---------------------------------------------------------------------------
# 6) Choose Components page. Category IDs remain English; only labels change.
# ---------------------------------------------------------------------------
p, original = load("app/src/main/java/com/winlator/cmod/ui/onboarding/OnboardingComponentsUi.kt")
text = original
text = replace_all(text, {
    '"Install and manage runtime versions."': '"安装和管理运行时版本。"',
    '"Install a Wine or Proton layer before continuing."': '"继续之前请至少安装一个 Wine 或 Proton 运行时。"',
    '"Install as many versions as you want. At least one Wine or Proton is required."': '"可安装多个版本，但至少需要一个 Wine 或 Proton 运行时。"',
    '"Wait for $bundledRuntimeName to finish installing, or install another Wine/Proton version."': '"请等待 $bundledRuntimeName 安装完成，或安装其他 Wine/Proton 版本。"',
    '"Install at least one Wine or Proton version to continue."': '"至少安装一个 Wine 或 Proton 版本后才能继续。"',
    '"Continue unlocks when $bundledRuntimeName finishes installing or another Wine/Proton layer is installed."': '"$bundledRuntimeName 安装完成或安装其他 Wine/Proton 后即可继续。"',
    'nextLabel = if (managerMode) "Done" else "Continue"': 'nextLabel = if (managerMode) "完成" else "继续"',
    '"No components available in this category."': '"此分类暂无可用组件。"',
    '"Winlator servers"': '"Winlator 在线源"',
    '"Local package"': '"本地安装包"',
    'busy -> "Working…"': 'busy -> "处理中…"',
    'installed && inUse -> "Bundled • Installed • In use"': 'installed && inUse -> "内置 • 已安装 • 使用中"',
    'installed -> "Bundled • Installed"': 'installed -> "内置 • 已安装"',
    '!ready.value -> "Bundled • Installing ${progress.value}%"': '!ready.value -> "内置 • 正在安装 ${progress.value}%"',
    'else -> "Bundled • Not installed"': 'else -> "内置 • 未安装"',
    '"${installingLabel ?: "Installing"} • ${installingProgress}%"': '"${installingLabel ?: "正在安装"} • ${installingProgress}%"',
    'installingLabel ?: "Working…"': 'installingLabel ?: "处理中…"',
    '"${item.type} • In use"': '"${item.type} • 使用中"',
    '"${label ?: "Installing"} • ${progress}%"': '"${label ?: "正在安装"} • ${progress}%"',
    'label ?: "Installing component…"': 'label ?: "正在安装组件…"',
})
text = text.replace('Text(it, Modifier.padding(horizontal = 13.dp, vertical = 8.dp), style = MaterialTheme.typography.labelLarge)',
                    'Text(componentCategoryLabel(it), Modifier.padding(horizontal = 13.dp, vertical = 8.dp), style = MaterialTheme.typography.labelLarge)')
cat_anchor = '@Composable\nprivate fun CategorySelector(selected: String, select: (String) -> Unit) {'
cat_helper = '''private fun componentCategoryLabel(value: String): String = when (value) {
    "Recommended" -> "推荐"
    "Wine & Proton" -> "Wine / Proton"
    "AdrenoTools" -> "Adreno 驱动"
    else -> value
}

'''
if cat_helper not in text:
    if cat_anchor not in text: raise RuntimeError("missing component category anchor")
    text = text.replace(cat_anchor, cat_helper + cat_anchor, 1)
save(p, original, text)

# ---------------------------------------------------------------------------
# 7) Regression guard for the exact pages reported by the user.
# ---------------------------------------------------------------------------
for rel, forbidden in {
    "app/src/main/java/com/winlator/cmod/ui/inputcontrols/InputControlsComposeHost.kt": [
        '"Input Controls"', '"EXTERNAL CONTROLLERS"', '"Overlay Opacity"', '"Remove controller"'
    ],
    "app/src/main/java/com/winlator/cmod/ui/library/BitmapComposeCompat.kt": [
        '"Library"', '"No games match your search"', '"Configure"', '"Remove from library"'
    ],
    "app/src/main/java/com/winlator/cmod/ui/onboarding/OnboardingComponentsUi.kt": [
        '"Winlator servers"', '"Local package"', '"No components available in this category."'
    ],
    "app/src/main/java/com/winlator/cmod/ui/container/ContainerEditorV2.kt": [
        '"Container settings"', '"Wallpaper image"', '"Default wallpaper"'
    ],
}.items():
    data = (ROOT / rel).read_text(encoding="utf-8")
    leaked = [s for s in forbidden if s in data]
    if leaked:
        raise RuntimeError(f"visible English regression in {rel}: {leaked}")

print("Patched files:")
for name in changes:
    print(" -", name)
print(f"Total: {len(changes)}")
