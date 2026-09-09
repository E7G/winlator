#!/usr/bin/env python3
"""Idempotently localize known hard-coded user-facing Java/Kotlin UI literals.

The replacement is intentionally restricted to lines containing UI APIs/components so internal
preference keys, IDs, command-line arguments and runtime protocol values are never globally
rewritten just because they share an English word.
"""
from pathlib import Path
import re

ROOT = Path("app/src/main/java")
QUOTED = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')

UI_MARKERS = (
    '.setText(', '.setHint(', '.setTitle(', '.setMessage(', '.setContentDescription(',
    '.setPositiveButton(', '.setNegativeButton(', '.setNeutralButton(', 'Toast.makeText(',
    'Text(', 'SectionTitle(', 'NavigationRow(', 'ToggleRow(', 'EditableValueCard(',
    'ActionButton(', 'StorageValue(', 'PreferenceCard(', 'SettingRow(', 'OptionRow(',
    'contentDescription =', 'label =', 'placeholder =', 'supportingText =', 'headlineContent =',
)

T = {
    # Common actions / states
    'Okay': '确定', 'OK': '确定', 'Cancel': '取消', 'Back': '返回', 'Close': '关闭',
    'Save': '保存', 'Delete': '删除', 'Remove': '移除', 'Rename': '重命名', 'Replace': '替换',
    'Edit': '编辑', 'More': '更多', 'Duplicate': '复制', 'Clone': '克隆', 'Apply': '应用',
    'Done': '完成', 'Add': '添加', 'Install': '安装', 'Download': '下载', 'Continue': '继续',
    'Play': '运行', 'Import': '导入', 'Export': '导出', 'Yes': '是', 'No': '否',
    'Help': '帮助', 'Properties': '属性', 'Value': '值', 'Name': '名称', 'Path': '路径',
    'System': '系统', 'None': '无', 'About': '关于', 'Appearance': '外观', 'Theme': '主题',

    # Java UI
    'Icon updated!': '图标已更新！',
    'Error saving icon': '保存图标失败',
    'Shortcut removed.': '快捷方式已删除。',
    'Cloned successfully.': '克隆成功。',
    'Connection failed!': '连接失败！',
    'Unable to parse driver updates.': '无法解析驱动更新。',
    'No driver updates found.': '未找到驱动更新。',
    'Downloading': '正在下载',
    'Download failed!': '下载失败！',
    'Installed:': '已安装：',
    'Installation failed! Invalid ZIP.': '安装失败：ZIP 文件无效。',
    'Preset saved': '预设已保存',
    'Unable to install': '无法安装',
    'cannot be deleted because it is used by': '因正在被以下项目使用而无法删除',
    'could not be deleted.': '删除失败。',
    'Unable to install the driver.': '无法安装驱动。',
    'Storage access is required to manage games.': '需要存储访问权限才能管理游戏。',
    'Install and select a Wine or Proton layer first.': '请先安装并选择 Wine 或 Proton 兼容层。',
    'The selected Wine/Proton layer is no longer installed.': '所选 Wine/Proton 兼容层已不再安装。',
    'Unable to create the first container.': '无法创建首个容器。',
    'Unable to prepare the first container.': '无法准备首个容器。',
    'No external storage found': '未找到外部存储',
    'RootFS not found': '未找到 RootFS',
    'Storage is not accessible': '无法访问存储',
    'Opened C: (': '已打开 C:（',
    "The Wine system files (Drive C:) for '": "Wine 系统文件（C: 盘）缺失：'",
    "' are missing.\\n\\n": "'。\\n\\n",
    'Drive root reached': '已到达驱动器根目录',
    'Folder is not accessible': '无法访问文件夹',
    'Create a container first!': '请先创建容器！',
    'No free drive letter for external storage': '没有可用于外部存储的空闲盘符',
    'Error launching:': '启动失败：',
    'Game added to Library!': '游戏已添加到游戏库！',
    'Cut:': '已剪切：',
    'Copied:': '已复制：',
    'Nothing to paste': '没有可粘贴的内容',
    'The destination \\"': '目标 \\"',
    '\\" already exists.': '\\" 已存在。',
    'Moved instantly': '已立即移动',
    'Success!': '成功！',
    'Cancelled': '已取消',
    'Error:': '错误：',
    '. Source preserved.': '。源文件已保留。',
    'Rename failed': '重命名失败',
    'Are you sure you want to delete': '确定要删除',
    'Sorry, your device may not be supported': '抱歉，您的设备可能不受支持',
    'Failed to add shortcut.': '添加快捷方式失败。',
    'Shortcut added successfully from BroadcastReceiver!': '已通过 BroadcastReceiver 成功添加快捷方式！',
    'Shortcut added successfully (Broadcast).': '已成功添加快捷方式（广播）。',
    'Failed to add shortcut via broadcast.': '通过广播添加快捷方式失败。',
    'MP3 reset to default': 'MP3 已恢复默认',
    'Times Played:': '游玩次数：',
    'Playtime:': '游玩时长：',
    'Not Set': '未设置',
    'No suitable cover art found for': '未找到适合的封面：',
    '. Click the image to upload custom cover art or rename the Shortcut to something SteamGrid can recognize.':
        '。点击图片可上传自定义封面，或将快捷方式重命名为 SteamGrid 能识别的名称。',
    'Invalid folder selected!': '选择的文件夹无效！',
    'No files in folder!': '文件夹中没有文件！',
    'No PNG files found in this folder!': '此文件夹中未找到 PNG 文件！',
    'Invalid directory selected': '选择的目录无效',
    'Processes:': '进程：',
    'PID:': 'PID：',
    'Change image': '更改图片', 'Choose image': '选择图片',
    'GB Used /': '已用 GB /', 'Total': '总计',
    'Add Source': '添加源', 'Add Repository': '添加仓库', 'Edit Repository': '编辑仓库',

    # Library / onboarding
    'View details': '查看详情', 'Add games': '添加游戏',
    'Winlator needs storage and notification access to manage games and keep sessions running.':
        'Winlator 需要存储和通知权限，以便管理游戏并保持会话运行。',
    'Choose environment': '选择运行环境', 'Preparing environment': '正在准备运行环境',
    'Preparing…': '正在准备…', 'Choose components': '选择组件',
    'Install local driver': '安装本地驱动', 'In use': '使用中',
    'Component installation': '组件安装', 'Loading component catalog…': '正在加载组件目录…',
    'Welcome to Winlator': '欢迎使用 Winlator',
    'Your lightweight PC emulator for Android.': '轻量高效的 Android PC 模拟器。',

    # Input controls
    'Controls Editor': '控制编辑器', '-- Select Profile --': '-- 选择配置 --',
    'No controllers connected': '未连接控制器',

    # Settings / components
    'Graphics Driver': '图形驱动', 'Wallpaper preview': '壁纸预览',
    'Desktop Background': '桌面背景', 'Image': '图片', 'Choose a version': '选择版本',
    'Downloading…': '正在下载…', 'Preset': '预设',
    'Container duplicated': '容器已复制', 'Remove container?': '移除容器？',
    'Container removed': '容器已移除', 'Containers': '容器', 'New container': '新建容器',
    'No containers': '没有容器', 'No containers yet': '暂无容器',
    'Create a Windows environment for your games.': '为你的游戏创建 Windows 运行环境。',
    'Container properties': '容器属性', 'Calculating storage…': '正在计算存储占用…',
    'Cache': '缓存', 'storage': '存储', 'Clearing…': '正在清理…', 'Clear cache': '清理缓存',
    'Add variable': '添加变量', 'Select values': '选择值', 'Add environment variable': '添加环境变量',
    'APPEARANCE': '外观', 'ENVIRONMENTS': '运行环境', 'PRESETS': '预设',
    'PATH SETTINGS': '路径设置', 'Winlator Path': 'Winlator 路径',
    'Shortcut Export Path': '快捷方式导出路径', 'BIG PICTURE MODE': '大屏模式',
    'Enable Big Picture Mode on App Launch': '应用启动时启用大屏模式',
    'Set SteamGrid API Key? (Cover Art)': '设置 SteamGrid API 密钥？（封面）',
    'SteamGridDB API Key': 'SteamGridDB API 密钥',
    'Capture External Pointer': '捕获外部指针',
    'Disable Xinput (Used for Exclusive M/KB support)': '禁用 XInput（用于独占鼠标/键盘支持）',
    'Downloadable Contents URL': '可下载组件地址', 'ABOUT': '关于',
    'Cursor speed': '光标速度', 'Create new': '新建',
    'Wine debug channels': 'Wine 调试通道', 'Search channels': '搜索通道',
    'Install SoundFont': '安装 SoundFont',

    # Container editor / compatibility
    'ALSA offers direct audio output. PulseAudio can improve compatibility in some applications.':
        'ALSA 提供直接音频输出；PulseAudio 可提升部分应用的兼容性。',
    '64-bit Emulator': '64 位模拟器', '32-bit Emulator': '32 位模拟器',
    'FEXCore Preset': 'FEXCore 预设', 'Box64 Preset': 'Box64 预设',
    'Present Mode': '呈现模式', 'Renderer Driver': '渲染驱动', 'Texture Filter': '纹理过滤',
    'Swap red/blue channels': '交换红/蓝通道', 'Driver Version': '驱动版本',
    'DXVK Version': 'DXVK 版本', 'VKD3D Version': 'VKD3D 版本',
    'VKD3D Feature Level': 'VKD3D 功能级别', 'Frame Rate Limit': '帧率限制',
    'DDraw Wrapper': 'DDraw 封装', 'Offscreen Rendering': '离屏渲染',
    'WineD3D Renderer': 'WineD3D 渲染器', 'Video Memory (MB)': '显存（MB）',
    'Manage installed versions': '管理已安装版本', 'Width': '宽度', 'Height': '高度',
    'Container name': '容器名称', 'Background color': '背景颜色', 'Target Path': '目标路径',
    'Launch Environment': '启动运行环境', 'DirectX wrapper': 'DirectX 封装',
    'Unable to create container.': '无法创建容器。', 'Creating…': '正在创建…',
    'Create container': '创建容器', 'Unable to set wallpaper image.': '无法设置壁纸图片。',
    'Change': '更改', 'Choose': '选择', 'Custom resolution': '自定义分辨率',
    'Frame Rate': '帧率', 'Disabled Vulkan Extensions': '已禁用的 Vulkan 扩展',
    'Vulkan Extensions': 'Vulkan 扩展', 'GPU Name': 'GPU 名称',
    'Blacklisted Extensions': '黑名单扩展', 'Video Memory': '显存', 'Runtime': '运行环境',
    'Advanced': '高级', 'Game Controller': '游戏控制器',
    'Processor Affinity': '处理器亲和性', 'Processor Affinity (32-bit apps)': '处理器亲和性（32 位应用）',

    # Shortcuts
    'Drive letters': '驱动器盘符', 'Unable to install $version': '无法安装 $version',
    'Unable to install ${option.label}': '无法安装 ${option.label}',
    'Shortcut copied to ${target.name}': '快捷方式已复制到 ${target.name}',
    'Environment': '运行环境', 'Enter container': '进入容器', 'Exec Arguments': '启动参数',
    '$enabledCount of ${extensions.size} enabled': '已启用 $enabledCount / ${extensions.size}',
}


def replace_line(line: str) -> tuple[str, int]:
    if not any(marker in line for marker in UI_MARKERS):
        return line, 0
    changed = 0

    def repl(match: re.Match) -> str:
        nonlocal changed
        raw = match.group(1)
        translated = T.get(raw)
        if translated is None:
            return match.group(0)
        changed += 1
        escaped = translated.replace('\\', '\\\\').replace('"', '\\"')
        return f'"{escaped}"'

    return QUOTED.sub(repl, line), changed


def main() -> None:
    files_changed = 0
    replacements = 0
    for path in sorted(list(ROOT.rglob('*.java')) + list(ROOT.rglob('*.kt'))):
        text = path.read_text(encoding='utf-8', errors='ignore')
        out_lines = []
        file_changes = 0
        for line in text.splitlines(keepends=True):
            new_line, count = replace_line(line)
            out_lines.append(new_line)
            file_changes += count
        if file_changes:
            path.write_text(''.join(out_lines), encoding='utf-8')
            files_changed += 1
            replacements += file_changes
            print(f'{path}: {file_changes}')
    print(f'Localized {replacements} UI literals across {files_changed} files.')


if __name__ == '__main__':
    main()
