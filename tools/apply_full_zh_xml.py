#!/usr/bin/env python3
from pathlib import Path
import hashlib
import html
import re

# Exact user-facing literals still hard-coded by upstream Ludashi 4.0 XML.
ZH = {
    'FPS:': 'FPS：', 'Renderer:': '渲染器：', 'GPU:': 'GPU：', 'RAM:': '内存：',
    'Left Analog Stick Settings': '左摇杆设置', 'Deadzone: 10%': '死区：10%',
    'Sensitivity: 100%': '灵敏度：100%', 'Use Square Deadzone (Left Stick)': '左摇杆使用方形死区',
    'Right Analog Stick Settings': '右摇杆设置', 'Invert X Axis (Left Stick)': '反转左摇杆 X 轴',
    'Invert Y Axis (Left Stick)': '反转左摇杆 Y 轴', 'Invert X Axis (Right Stick)': '反转右摇杆 X 轴',
    'Invert Y Axis (Right Stick)': '反转右摇杆 Y 轴', 'Number of times played:': '游玩次数：',
    'Playtime:': '游玩时长：', 'Reset Properties': '重置属性', 'Storage': '存储', 'Settings': '设置',
    'Graphics Driver:': '图形驱动：', 'Driver Version:': '驱动版本：', 'DX Wrapper:': 'DX 转译层：',
    'DXVK/VKD3D Config:': 'DXVK/VKD3D 配置：', 'Audio Driver:': '音频驱动：', 'Box64 Preset:': 'Box64 预设：',
    'Nothing here. Launch your container and create a Shortcut to get started!': '这里还没有内容。请启动容器并创建快捷方式。',
    'Back': '返回', 'YouTube Video URL (Not all YouTube Videos are supported):': 'YouTube 视频地址（并非所有视频都受支持）：',
    'Enter YouTube URL': '输入 YouTube 地址', 'Set BG Music': '设置背景音乐', 'Select MP3': '选择 MP3',
    'Reset MP3': '重置 MP3', 'Select Music Source:': '选择音乐来源：', 'Disable BG Music': '关闭背景音乐',
    'Background settings:': '背景设置：', 'Custom Wallpaper': '自定义壁纸', 'Select Wallpaper': '选择壁纸',
    'No Background': '无背景', 'Wave': '波浪', 'Gear': '齿轮', 'Quilt': '拼布', 'Folder': '文件夹',
    'Select Animation (*.png) Folder': '选择动画（*.png）文件夹', 'Parallax Mode:': '视差模式：',
    'Off': '关闭', 'Slow': '慢', 'Default': '默认', 'Fast': '快', 'Animation Speed:': '动画速度：',
    'Present Mode': '呈现模式', 'Renderer Driver': '渲染驱动', 'Texture Filter': '纹理过滤',
    'Swap red/blue channels': '交换红/蓝通道', 'APP SETTINGS': '应用设置', 'Runtime and interface': '运行环境与界面',
    'Presets, paths, input and diagnostics': '预设、路径、输入与诊断', 'Enable Dark Mode': '启用深色模式',
    'Theme': '主题', 'Winlator Path': 'Winlator 路径', 'Choose Path': '选择路径',
    'Shortcut Export Path': '快捷方式导出路径', 'Path Settings': '路径设置',
    'Enable Big Picture Mode on App Launch': '启动应用时进入大屏模式',
    'Set SteamGrid API Key? (Cover Art)': '设置 SteamGrid API 密钥？（封面图）',
    'Enter your API Key here': '在此输入 API 密钥', 'SteamGridDB API Key': 'SteamGridDB API 密钥',
    'Big Picture Mode': '大屏模式', 'Capture External Pointer': '捕获外接鼠标指针',
    'Disable Xinput (Used for Exclusive M/KB support)': '禁用 XInput（用于独占鼠标/键盘支持）',
    'Downloadable Contents URL': '可下载内容地址', 'Up Directory': '返回上级目录', 'Paste': '粘贴',
    'Rendering': '渲染', 'FPS Limiter': 'FPS 限制', 'Super Resolution': '超分辨率',
    'Upscaler Mode': '升频模式', 'Sharpness': '锐度', 'Post Effect': '后处理效果', 'ReShade': 'ReShade',
    'Effect': '效果', 'Strength': '强度', 'Frame Generation': '帧生成', 'Save Preset': '保存预设',
    'Display and Effects': '显示与效果', 'Picture in Picture': '画中画', 'Toggle Fullscreen': '切换全屏',
    'Magnifier': '放大镜', 'Soft Stretch': '柔性拉伸', 'Controls': '控制',
    'Touch Controls Opacity': '触控按键透明度', 'Show Keyboard': '显示键盘', 'Vibration': '振动',
    'Relative Mouse': '相对鼠标', 'Disable Mouse': '禁用鼠标', 'HUD': 'HUD', 'Enable HUD': '启用 HUD',
    'Style': '样式', 'HUD Metrics': 'HUD 指标', 'FPS': 'FPS', 'GPU': 'GPU', 'CPU': 'CPU', 'RAM': '内存',
    'Batt/Temp': '电量/温度', 'Renderer': '渲染器', 'HUD Size': 'HUD 大小', 'HUD Opacity': 'HUD 透明度',
    'Reset HUD': '重置 HUD', 'Show Logs': '显示日志', 'Task Manager': '任务管理器', 'Memory': '内存',
    'Processes': '进程', '+ New Task': '+ 新建任务', 'Max Frame Latency': '最大帧延迟',
    'Installed Driver': '已安装驱动', 'Latest StevenMXZ Drivers': '最新 StevenMXZ 驱动', 'Refresh': '刷新',
    'Driver Repositories': '驱动仓库', 'Manage driver sources and updates.': '管理驱动来源与更新。',
    'Install Driver from File': '从文件安装驱动', 'More options': '更多选项', 'Current Version:': '当前版本：',
    '64bit Emulator': '64 位模拟器', 'Enable Relative Mouse': '启用相对鼠标', 'Exclusive Input': '独占输入',
    'General': '常规', 'Environment': '环境', 'Drivers': '驱动', 'Components': '组件', 'DirectInput': 'DirectInput',
    'Enter command': '输入命令', 'Execute': '执行', 'Opacity': '透明度', 'Theme Color': '主题颜色',
    'Remove': '删除', 'Browse…': '浏览…', 'Move mouse cursor while dragging': '拖动时移动鼠标光标',
    'Click to upload custom cover art': '点击上传自定义封面', 'File Manager': '文件管理器',
    'Toggle Relative Mouse Movement': '切换相对鼠标移动', 'Pause/Resume': '暂停/继续', 'Change Icon': '更改图标',
    'Export': '导出', 'Clone': '克隆',
}

# Java strings caught by direct user-facing UI setters. These are translated in-place to avoid changing
# overload behavior of setText/setTitle/setMessage in the many different UI classes used by this fork.
JAVA_ZH = {
    'All Files Access Required': '需要所有文件访问权限',
    'In order to grant access to additional storage devices such as USB storage device, the All Files Access permission must be granted. Press Okay to grant All Files Access in your Android Settings.': '为了访问 USB 存储等额外存储设备，需要授予“所有文件访问”权限。点击“确定”前往 Android 设置授权。',
    'Scheme Color': '方案颜色',
    "Applies to every control that doesn't have its own custom color": '应用于所有未设置自定义颜色的控制项',
    'Select a container': '选择容器', 'Current Driver': '当前驱动', 'Proton is in use': 'Proton 正在使用',
    'The bundled Proton files will be removed. You can install them again later.': '内置 Proton 文件将被删除，之后可以重新安装。',
    'Delete driver?': '删除驱动？', 'The installed driver files will be removed.': '已安装的驱动文件将被删除。',
    'Runtime is in use': '运行环境正在使用', 'Delete component?': '删除组件？',
    'The installed files will be removed from WinZ.': '已安装文件将从 WinZ 中删除。',
    'File Manager': '文件管理器', 'No Containers': '没有容器',
    'You need to create a container first to access Drive C:.': '需要先创建容器才能访问 C: 盘。',
    'Select Container Drive C:': '选择容器 C: 盘', 'Drive C: Not Initialized': 'C: 盘尚未初始化',
    'Drive C:': 'C: 盘', 'External Storage': '外部存储', 'Drive Z:': 'Z: 盘', 'Select Container': '选择容器',
    'File Conflict': '文件冲突', 'Calculating...': '正在计算…', 'Rename': '重命名', 'Delete': '删除',
    'Disable BG Music': '关闭背景音乐', 'Enable BG Music': '启用背景音乐', 'Cover Art Options': '封面选项',
    'Not Set': '未设置', 'Select Display Mode': '选择显示模式', 'FPS Limit': 'FPS 限制',
    'Enter custom FPS': '输入自定义 FPS', 'Off': '关闭', 'Custom': '自定义', 'Dual-cell correction': '双单元校正',
    'ReShade effect': 'ReShade 效果', 'Wallpaper': '壁纸', 'Wallpaper image': '壁纸图片',
    'Available Drivers': '可用驱动', 'Select Variant': '选择版本', 'Driver Sources': '驱动源',
    'Name (e.g. Turnip Drivers)': '名称（例如 Turnip 驱动）', 'GitHub API URL': 'GitHub API 地址',
}

MISSING_ZH = {
    'icon_set_successfully': '图标设置成功', 'unable_to_set_icon': '无法设置图标', 'library': '游戏库',
    'configure_container': '配置容器', 'launch_environment': '启动环境', 'video': '视频', 'storage': '存储',
    'quick_settings': '快捷设置', 'advanced_settings': '高级设置', 'global_compatibility_profile': '全局兼容性配置',
    'profile_compatibility': '兼容性', 'profile_balanced': '均衡', 'profile_performance': '性能',
    'profile_custom': '自定义', 'play': '运行', 'arguments': '启动参数', 'game_folder': '游戏文件夹',
    'magnifier_not_available': 'DisplayX 模式下无法使用放大镜',
}

SKIP = {
    'Winlator', 'Winlator CMOD', 'YouTube', 'Drive D:', '/storage/emulated/0/Download', 'FolderName', 'default_version',
    '-force-gfx-direct', '-force-d3d11-singlethreaded', '-force-dx9', '-force-d3d9', '-force-d3d11',
    '--force-gfx-direct', '--force-d3d11-singlethreaded', '--force-dx9', '--force-d3d9', '--force-d3d11',
}

ATTR_RE = re.compile(r'(android:(?:text|hint|title|summary|contentDescription)=")([^"]+)(")')
STR_RE = re.compile(r'<string\s+name="([^"]+)"[^>]*>(.*?)</string>', re.S)
JAVA_UI_RE = re.compile(r'(\.(?:setText|setHint|setTitle|setMessage|setContentDescription)\(\s*")([^"\\]*(?:\\.[^"\\]*)*)("\s*\))')

def key_for(text):
    slug = re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')
    slug = slug[:48] if slug else 'text'
    return f'zh_hc_{slug}_{hashlib.sha1(text.encode("utf-8")).hexdigest()[:8]}'

def read_strings(path):
    if not path.exists():
        return {}
    text = path.read_text(encoding='utf-8', errors='ignore')
    return {m.group(1): m.group(2).strip() for m in STR_RE.finditer(text)}

base_out = Path('app/src/main/res/values/strings_zhlocalize.xml')
zh_out = Path('app/src/main/res/values-zh-rCN/strings_ludashi4.xml')
existing_base = {k: v for k, v in read_strings(base_out).items() if k.startswith('zh_hc_')}
existing_zh = {k: v for k, v in read_strings(zh_out).items() if k.startswith('zh_hc_')}
used = {k: (html.unescape(v), html.unescape(existing_zh.get(k, v))) for k, v in existing_base.items()}

changed_files = 0
replacements = 0
for root in [Path('app/src/main/res/layout'), Path('app/src/main/res/layout-land'), Path('app/src/main/res/menu'), Path('app/src/main/res/xml')]:
    if not root.exists():
        continue
    for p in root.rglob('*.xml'):
        original = p.read_text(encoding='utf-8')
        def repl(m):
            global replacements
            value = m.group(2)
            lookup = value.strip()
            if value.startswith(('@', '?')) or lookup in SKIP or lookup not in ZH:
                return m.group(0)
            key = key_for(lookup)
            used[key] = (lookup, ZH[lookup])
            replacements += 1
            return m.group(1) + '@string/' + key + m.group(3)
        updated = ATTR_RE.sub(repl, original)
        if updated != original:
            p.write_text(updated, encoding='utf-8')
            changed_files += 1

java_changed = 0
java_replacements = 0
for p in Path('app/src/main/java').rglob('*.java'):
    original = p.read_text(encoding='utf-8', errors='ignore')
    def java_repl(m):
        global java_replacements
        value = m.group(2)
        if value not in JAVA_ZH:
            return m.group(0)
        java_replacements += 1
        return m.group(1) + JAVA_ZH[value].replace('"', '\\"') + m.group(3)
    updated = JAVA_UI_RE.sub(java_repl, original)
    if updated != original:
        p.write_text(updated, encoding='utf-8')
        java_changed += 1

base_out.parent.mkdir(parents=True, exist_ok=True)
zh_out.parent.mkdir(parents=True, exist_ok=True)
base_lines = ['<?xml version="1.0" encoding="utf-8"?>', '<resources>']
for key, (en, cn) in sorted(used.items()):
    base_lines.append(f'    <string name="{key}" formatted="false">{html.escape(en, quote=False)}</string>')
base_lines.append('</resources>')
base_out.write_text('\n'.join(base_lines) + '\n', encoding='utf-8')

zh_lines = ['<?xml version="1.0" encoding="utf-8"?>', '<resources>']
for key, cn in MISSING_ZH.items():
    zh_lines.append(f'    <string name="{key}">{html.escape(cn, quote=False)}</string>')
for key, (en, cn) in sorted(used.items()):
    zh_lines.append(f'    <string name="{key}" formatted="false">{html.escape(cn, quote=False)}</string>')
zh_lines.append('</resources>')
zh_out.write_text('\n'.join(zh_lines) + '\n', encoding='utf-8')

print(f'Localized {replacements} XML attributes across {changed_files} files; retained/generated {len(used)} XML resource keys.')
print(f'Localized {java_replacements} Java UI literals across {java_changed} files.')
